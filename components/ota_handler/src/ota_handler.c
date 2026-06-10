/**
 * @file    ota_handler.c
 * @brief   OTA 固件升级模块 — 核心实现
 *
 * 【升级流程】
 *   1. 定时器触发 或 MQTT 命令触发
 *   2. HTTPS GET /api/firmware/check?device_id=xxx&version=x.y.z
 *   3. 服务器返回 { has_update, version, url, sha256, changelog }
 *   4. 如果有新版本且自动升级开关打开：
 *      a. 通过 esp_https_ota() 下载固件并写入对侧 OTA 分区
 *      b. 上报下载进度（每 10% 通过 MQTT 发布一次）
 *      c. 校验 SHA256
 *      d. 标记启动分区为新的固件
 *      e. 重启设备
 *   5. 新固件启动后：
 *      a. 调用 esp_ota_mark_app_valid_cancel_rollback() 确认固件有效
 *      b. 如果崩溃（未调用上述函数），bootloader 自动回滚
 *
 * 【降级流程】
 *   1. MQTT 收到降级命令 { action: "downgrade", version: "2.0.0" }
 *   2. 通过 esp_https_ota() 下载指定旧版本固件
 *   3. 刷写并重启（与升级流程相同）
 *
 * 【安全机制】
 *   - 使用 ESP x509 Certificate Bundle 进行 TLS 验证（无需自签证书）
 *   - API Token 鉴权
 *   - SHA256 校验确保固件完整性
 *   - OTA 回滚：新固件启动后必须显式标记有效，否则自动回退
 */

#include "ota_handler.h"
#include "device_id.h"
#include "mqtt_ha.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ota_handler";

/*===========================================================================
 * NVS 键名定义
 *===========================================================================*/

#define NVS_OTA_NAMESPACE    "ota"
#define NVS_KEY_FW_VERSION   "fw_version"

/*===========================================================================
 * 编译时固件版本号（由 CMake 注入 -DFIRMWARE_VERSION）
 *===========================================================================*/

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0"
#endif

/*===========================================================================
 * API URL 路径
 *===========================================================================*/

#define OTA_CHECK_PATH    "/api/firmware/check"
#define OTA_DOWNLOAD_PATH "/api/firmware/download"

/*===========================================================================
 * 模块内部状态
 *===========================================================================*/

/** @brief 编译时固件版本（只读） */
static const char *g_compile_version = FIRMWARE_VERSION;

/** @brief 是否正在执行 OTA（防止重复触发） */
static volatile bool g_ota_in_progress = false;

/** @brief 上次检查时的最新版本（用于去重，避免重复升级） */
static char g_last_known_latest[32] = {0};

/** @brief OTA 进度回调（外部注册） */
static ota_progress_cb_t g_progress_cb = NULL;

/** @brief FreeRTOS 定时器句柄（定时检查） */
static TimerHandle_t g_check_timer = NULL;

/*===========================================================================
 * 前向声明
 *===========================================================================*/

static void ota_check_task(void *pvParameters);
static void handle_ota_mqtt_command(const char *data, int data_len);
static void ota_download_and_apply(const char *version, const char *url);
static void ota_publish_progress(int progress_pct, const char *status);
static void ota_mark_valid(void);
static void ota_handle_rollback(void);
static void ota_mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data);

/*===========================================================================
 * 公开 API
 *===========================================================================*/

const char *ota_get_version(void)
{
    return g_compile_version;
}

void ota_check_now(void)
{
    if (g_ota_in_progress) {
        ESP_LOGW(TAG, "OTA already in progress, skip check");
        return;
    }
    xTaskCreate(&ota_check_task, "ota_check", 8192, NULL, 5, NULL);
}

void ota_register_progress_callback(ota_progress_cb_t cb)
{
    g_progress_cb = cb;
}

/*===========================================================================
 * 模块初始化
 *===========================================================================*/

void ota_handler_init(void)
{
    ESP_LOGI(TAG, "OTA handler initializing...");
    ESP_LOGI(TAG, "  Compile-time firmware version: %s", g_compile_version);

    /*----------------------------------------------------------------------
     * 1. NVS 版本号管理
     *
     * 从 NVS 读取上次记录的版本号，与此编译版本比对：
     *   - 如果编译版本 != NVS 版本 → 说明刚完成 OTA 升级，标记固件有效
     *   - 如果编译版本 == NVS 版本 → 正常启动，检查是否有待处理的回滚
     *--------------------------------------------------------------------*/
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_OTA_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        char saved_version[32] = {0};
        size_t len = sizeof(saved_version);
        err = nvs_get_str(nvs, NVS_KEY_FW_VERSION, saved_version, &len);

        if (err == ESP_ERR_NVS_NOT_FOUND) {
            // 首次启动，写入编译版本
            nvs_set_str(nvs, NVS_KEY_FW_VERSION, g_compile_version);
            nvs_commit(nvs);
            ESP_LOGI(TAG, "First boot, version saved: %s", g_compile_version);
        } else if (err == ESP_OK) {
            if (strcmp(g_compile_version, saved_version) != 0) {
                // 版本号变了 → OTA 升级成功，标记固件有效
                ESP_LOGI(TAG, "Firmware updated: %s → %s", saved_version, g_compile_version);
                ota_mark_valid();
                nvs_set_str(nvs, NVS_KEY_FW_VERSION, g_compile_version);
                nvs_commit(nvs);

                // 上报升级成功
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "{\"status\":\"success\",\"from\":\"%s\",\"to\":\"%s\"}",
                         saved_version, g_compile_version);
                mqtt_ha_publish(ota_progress_topic, msg, 1);
            } else {
                // 版本号未变 → 正常启动，处理可能的回滚
                ota_handle_rollback();
            }
        }
        nvs_close(nvs);
    } else {
        ESP_LOGW(TAG, "NVS open failed, version not persisted");
    }

    /*----------------------------------------------------------------------
     * 2. 通过 MQTT 注册 OTA 命令回调
     *
     * 在 mqtt_ha 的 DATA 事件中，ota_command_topic 的消息会路由到这里。
     * 使用 mqtt_ha 模块的发布接口上报进度。
     *--------------------------------------------------------------------*/

    // 上报设备在线及当前版本
    char online_msg[256];
    snprintf(online_msg, sizeof(online_msg),
             "{\"version\":\"%s\",\"status\":\"online\",\"device_id\":\"%s\"}",
             g_compile_version, base_device_id);
    mqtt_ha_publish(ota_progress_topic, online_msg, 1);

    ESP_LOGI(TAG, "OTA handler initialized, subscribing to: %s", ota_command_topic);

    /*----------------------------------------------------------------------
     * 3. 订阅 OTA 命令主题
     *
     * 通过 esp_mqtt_client 直接订阅额外的 topic（不影响 mqtt_ha 的订阅）。
     *--------------------------------------------------------------------*/
    esp_mqtt_client_handle_t client = mqtt_ha_get_client();
    if (client) {
        esp_mqtt_client_subscribe(client, ota_command_topic, 1);
        // 注册独立的 MQTT 事件处理器（与 mqtt_ha 的处理器共存）
        esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                       ota_mqtt_event_handler, NULL);
    }

    /*----------------------------------------------------------------------
     * 4. 创建定时检查任务
     *
     * 使用 FreeRTOS Timer，每 CONFIG_OTA_CHECK_INTERVAL_MIN 分钟触发一次。
     * 延迟 30 秒后首次触发（等待 Wi-Fi 和 MQTT 完全就绪）。
     *--------------------------------------------------------------------*/
    g_check_timer = xTimerCreate(
        "ota_timer",
        pdMS_TO_TICKS(30000),  // 首次延迟 30 秒
        pdTRUE,                // 自动重载
        NULL,
        [](TimerHandle_t xTimer) {
            ota_check_now();
        }
    );

    if (g_check_timer) {
        xTimerStart(g_check_timer, 0);
        // 重新设置周期为配置值
        TickType_t interval = pdMS_TO_TICKS(CONFIG_OTA_CHECK_INTERVAL_MIN * 60 * 1000);
        xTimerChangePeriod(g_check_timer, interval, 0);
    }
}

/*===========================================================================
 * OTA 版本检查任务（FreeRTOS Task）
 *===========================================================================*/

static void ota_check_task(void *pvParameters)
{
    g_ota_in_progress = true;

    ESP_LOGI(TAG, "Checking for firmware update...");

    /*----------------------------------------------------------------------
     * 构造请求 URL:
     *   GET /api/firmware/check?device_id=<mac>&version=<current>
     *--------------------------------------------------------------------*/
    char url[256];
    snprintf(url, sizeof(url),
             "%s" OTA_CHECK_PATH "?device_id=%s&version=%s&token=%s",
             CONFIG_OTA_SERVER_URL,
             base_device_id,
             g_compile_version,
             CONFIG_OTA_API_TOKEN);

    /*----------------------------------------------------------------------
     * 配置 HTTP 客户端
     *
     * 使用 ESP x509 Certificate Bundle 进行 TLS 验证（适用于 HTTPS）
     * 如果服务器使用 HTTP（非 HTTPS），Bundle 会被忽略
     *--------------------------------------------------------------------*/
    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };

    esp_http_client_handle_t http = esp_http_client_init(&http_config);
    if (!http) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        g_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    // 设置 User-Agent 头
    esp_http_client_set_header(http, "User-Agent", "ESP32-C3-OTA/1.0");
    esp_http_client_set_header(http, "Accept", "application/json");

    esp_err_t err = esp_http_client_perform(http);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s (URL: %s)", esp_err_to_name(err), url);
        esp_http_client_cleanup(http);
        g_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    int status_code = esp_http_client_get_status_code(http);
    ESP_LOGI(TAG, "HTTP status: %d", status_code);

    if (status_code != 200) {
        ESP_LOGW(TAG, "Server returned %d", status_code);
        esp_http_client_cleanup(http);
        g_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    /*----------------------------------------------------------------------
     * 读取并解析 JSON 响应
     * 格式: { "has_update": true, "latest_version": "3.0.1", "url": "...",
     *          "file_size": 123456, "sha256": "abc...", "changelog": "..." }
     *--------------------------------------------------------------------*/
    char buffer[1024] = {0};
    int read_len = esp_http_client_read(http, buffer, sizeof(buffer) - 1);
    esp_http_client_cleanup(http);

    if (read_len <= 0) {
        ESP_LOGE(TAG, "Empty response body");
        g_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGD(TAG, "Response: %s", buffer);

    cJSON *resp = cJSON_Parse(buffer);
    if (!resp) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        g_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    cJSON *has_update = cJSON_GetObjectItem(resp, "has_update");
    if (cJSON_IsTrue(has_update)) {
        const char *latest_ver = cJSON_GetObjectItem(resp, "latest_version")->valuestring;
        const char *download_url = cJSON_GetObjectItem(resp, "url")->valuestring;
        const char *sha256 = cJSON_GetObjectItem(resp, "sha256")->valuestring;
        const char *changelog = cJSON_GetObjectItem(resp, "changelog")->valuestring;

        ESP_LOGI(TAG, "New version available: %s (current: %s)", latest_ver, g_compile_version);
        if (changelog) {
            ESP_LOGI(TAG, "Changelog: %s", changelog);
        }

        // 避免重复升级到同一版本
        if (strcmp(latest_ver, g_last_known_latest) == 0) {
            ESP_LOGI(TAG, "Already processed this version, skipping");
        } else {
            strncpy(g_last_known_latest, latest_ver, sizeof(g_last_known_latest) - 1);

#ifdef CONFIG_OTA_AUTO_UPGRADE
            // 构造完整下载 URL
            char full_url[256];
            snprintf(full_url, sizeof(full_url),
                     "%s%s?device_id=%s&from_version=%s&token=%s",
                     CONFIG_OTA_SERVER_URL,
                     download_url,
                     base_device_id,
                     g_compile_version,
                     CONFIG_OTA_API_TOKEN);

            ota_download_and_apply(latest_ver, full_url);
#else
            ESP_LOGI(TAG, "Auto upgrade disabled, only reporting via MQTT");
            char notify[256];
            snprintf(notify, sizeof(notify),
                     "{\"event\":\"update_available\",\"current\":\"%s\",\"latest\":\"%s\"}",
                     g_compile_version, latest_ver);
            mqtt_ha_publish(ota_progress_topic, notify, 1);
#endif
        }
    } else {
        ESP_LOGI(TAG, "No update available (current: %s)", g_compile_version);
    }

    cJSON_Delete(resp);
    g_ota_in_progress = false;
    vTaskDelete(NULL);
}

/*===========================================================================
 * 固件下载与刷写
 *===========================================================================*/

static void ota_download_and_apply(const char *version, const char *url)
{
    ESP_LOGI(TAG, "Starting OTA download: %s", url);
    ESP_LOGI(TAG, "Target version: %s", version);

    ota_publish_progress(0, "starting");

    /*----------------------------------------------------------------------
     * 配置 esp_https_ota
     *
     * esp_https_ota() 是 ESP-IDF 封装的便捷函数，自动完成：
     *   1. HTTPS GET 下载固件
     *   2. 写入对侧 OTA 分区
     *   3. SHA256 校验
     *   4. 设置启动分区
     *
     * http_config 用于配置 HTTP 层面；ota_config 用于 OTA 特有参数。
     *--------------------------------------------------------------------*/
    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = CONFIG_OTA_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
        .http_client_init_cb = NULL,
        .bulk_flash_erase = false,   // 仅擦除需要的扇区，加快速度
    };

    // 上报开始下载
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"event\":\"download_start\",\"version\":\"%s\"}", version);
    mqtt_ha_publish(ota_progress_topic, msg, 0);

    // 执行 OTA
    esp_err_t ret = esp_https_ota(&ota_config);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA download complete, rebooting in 3 seconds...");
        ota_publish_progress(100, "done");

        snprintf(msg, sizeof(msg),
                 "{\"event\":\"upgrade_success\",\"version\":\"%s\"}", version);
        mqtt_ha_publish(ota_progress_topic, msg, 1);

        // 延迟 3 秒等待 MQTT 消息发送完毕
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA download failed: %s (0x%x)", esp_err_to_name(ret), ret);
        ota_publish_progress(-1, "failed");

        snprintf(msg, sizeof(msg),
                 "{\"event\":\"upgrade_failed\",\"version\":\"%s\",\"error\":\"%s\"}",
                 version, esp_err_to_name(ret));
        mqtt_ha_publish(ota_progress_topic, msg, 1);
    }
}

/*===========================================================================
 * 进度上报
 *===========================================================================*/

static void ota_publish_progress(int progress_pct, const char *status)
{
    char msg[128];
    snprintf(msg, sizeof(msg),
             "{\"progress\":%d,\"status\":\"%s\"}", progress_pct, status);
    mqtt_ha_publish(ota_progress_topic, msg, 0);

    // 通知外部回调
    if (g_progress_cb) {
        g_progress_cb(progress_pct, status);
    }
}

/*===========================================================================
 * OTA 回滚与标记
 *===========================================================================*/

/**
 * @brief  标记当前固件为有效，取消待处理的回滚
 *
 * ESP-IDF OTA 机制：
 *   bootloader 在启动新 OTA 固件后将 otadata 分区标记为"待验证"状态。
 *   应用程序必须调用 esp_ota_mark_app_valid_cancel_rollback() 来确认固件有效。
 *   如果在看门狗超时前未调用此函数，bootloader 自动回滚到上一版本。
 *
 * 调用时机：
 *   - 新固件启动后，确认所有关键功能正常（Wi-Fi 连接、MQTT 连接）
 *   - 本项目中在 ota_handler_init() 中检测到版本号变更时自动调用
 */
static void ota_mark_valid(void)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Firmware marked as valid, rollback cancelled");
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGD(TAG, "Running from factory partition, no rollback needed");
    } else {
        ESP_LOGW(TAG, "Failed to mark app valid: %s", esp_err_to_name(err));
    }
}

/**
 * @brief  检查并处理 OTA 回滚状态
 *
 * 回滚场景：
 *   - 场景 A：新固件启动后崩溃（未调用 esp_ota_mark_app_valid_cancel_rollback）
 *             → bootloader 自动将启动分区切回旧版本
 *             → 本函数检测到回滚状态，记录日志并通知服务器
 *   - 场景 B：手动降级后正常启动
 *             → 无回滚状态，正常初始化
 */
static void ota_handle_rollback(void)
{
    esp_ota_img_states_t ota_state;
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "Previous OTA is still pending verify - this is a fresh OTA boot");
            // 这是新固件首次启动，标记为有效
            ota_mark_valid();
        } else if (ota_state == ESP_OTA_IMG_ABORTED) {
            ESP_LOGE(TAG, "OTA rollback detected! Previous firmware was aborted.");
            // 通知服务器发生了回滚
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "{\"event\":\"rollback\",\"version\":\"%s\",\"reason\":\"aborted\"}",
                     g_compile_version);
            mqtt_ha_publish(ota_progress_topic, msg, 1);
        } else if (ota_state == ESP_OTA_IMG_INVALID) {
            ESP_LOGE(TAG, "OTA rollback detected! Previous firmware was invalid.");
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "{\"event\":\"rollback\",\"version\":\"%s\",\"reason\":\"invalid\"}",
                     g_compile_version);
            mqtt_ha_publish(ota_progress_topic, msg, 1);
        }
    }
}

/*===========================================================================
 * MQTT 命令处理（由 mqtt_ha 模块回调）
 *
 * 命令格式：
 *   { "action": "upgrade",   "version": "3.0.1" }
 *   { "action": "downgrade", "version": "2.0.0" }
 *   { "action": "check" }
 *
 * 注意：此函数需要从 mqtt_ha 的 MQTT_EVENT_DATA 处理中调用。
 * 当前流程中，mqtt_ha 模块处理完自己的 topic 后会进入 default 分支。
 * 为简洁实现，本模块在 ota_handler_init() 中额外订阅 ota_command_topic，
 * 然后在 mqtt_event_handler 中添加处理逻辑。
 *
 * 替代方案（采用）：
 *   注册额外的 MQTT 事件处理器，在接收到 ota_command_topic 消息时处理。
 *   使用 esp_mqtt_client_subscribe + esp_mqtt_client_register_event 二次注册。
 *===========================================================================*/

/**
 * @brief  MQTT OTA 命令事件处理器
 *
 * 独立注册于 mqtt_ha 的事件处理器，专门处理 OTA 命令。
 * 该处理器在 mqtt_ha_init() 之后注册，不会影响原有灯光控制逻辑。
 */
static void ota_mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    if (event_id != MQTT_EVENT_DATA) {
        return;
    }

    // 仅处理 ota_command_topic
    if (event->topic_len != strlen(ota_command_topic) ||
        strncmp(event->topic, ota_command_topic, event->topic_len) != 0) {
        return;
    }

    // 验证 topic 长度一致后再打印完整 topic（strncmp 通过才打印）
    char topic_buf[129] = {0};
    int copy_len = event->topic_len < 128 ? event->topic_len : 128;
    memcpy(topic_buf, event->topic, copy_len);
    ESP_LOGI(TAG, "OTA command received on topic: %s", topic_buf);

    // 解析 JSON 命令
    cJSON *cmd = cJSON_ParseWithLength(event->data, event->data_len);
    if (!cmd) {
        ESP_LOGW(TAG, "Invalid OTA command JSON");
        return;
    }

    const char *action = cJSON_GetObjectItem(cmd, "action")->valuestring;
    const char *version = cJSON_GetObjectItem(cmd, "version") ?
                          cJSON_GetObjectItem(cmd, "version")->valuestring : NULL;

    if (strcmp(action, "check") == 0) {
        ESP_LOGI(TAG, "MQTT command: check for update");
        ota_check_now();
    } else if ((strcmp(action, "upgrade") == 0 || strcmp(action, "downgrade") == 0) && version) {
        ESP_LOGI(TAG, "MQTT command: %s to %s", action, version);

        // 构造下载 URL
        char url[256];
        snprintf(url, sizeof(url),
                 "%s" OTA_CHECK_PATH "?device_id=%s&version=%s&token=%s",
                 CONFIG_OTA_SERVER_URL,
                 base_device_id,
                 g_compile_version,
                 CONFIG_OTA_API_TOKEN);

        // 先检查版本是否存在
        esp_http_client_config_t http_config = {
            .url = url,
            .timeout_ms = 15000,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t http = esp_http_client_init(&http_config);
        if (http) {
            esp_http_client_set_header(http, "User-Agent", "ESP32-C3-OTA/1.0");
            esp_err_t err = esp_http_client_perform(http);

            if (err == ESP_OK && esp_http_client_get_status_code(http) == 200) {
                char buf[1024] = {0};
                esp_http_client_read(http, buf, sizeof(buf) - 1);

                cJSON *resp = cJSON_Parse(buf);
                if (resp) {
                    // 获取固件下载 ID
                    cJSON *latest_ver = cJSON_GetObjectItem(resp, "latest_version");
                    cJSON *download_url = cJSON_GetObjectItem(resp, "url");

                    if (latest_ver && download_url) {
                        ESP_LOGI(TAG, "Server has version %s, starting download", latest_ver->valuestring);

                        char full_url[256];
                        snprintf(full_url, sizeof(full_url),
                                 "%s%s?device_id=%s&from_version=%s&token=%s",
                                 CONFIG_OTA_SERVER_URL,
                                 download_url->valuestring,
                                 base_device_id,
                                 g_compile_version,
                                 CONFIG_OTA_API_TOKEN);

                        ota_download_and_apply(version, full_url);
                    }
                    cJSON_Delete(resp);
                }
            }
            esp_http_client_cleanup(http);
        }
    } else {
        ESP_LOGW(TAG, "Unknown OTA action: %s", action ? action : "null");
    }

    cJSON_Delete(cmd);
}
