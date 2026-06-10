/**
 * @file    wifi_manager.c
 * @brief   Wi-Fi 连接管理器 — 实现
 *
 * 完整的 Wi-Fi 管理流程：
 *
 *   【场景一：NVS 中有已保存凭据】
 *     上电 → load_wifi_credentials() → 设置 STA 配置 → esp_wifi_start()
 *     → esp_wifi_connect() → DHCP 获取 IP →
 *     IP_EVENT_STA_GOT_IP → 设置 WIFI_CONNECTED_BIT → 连接成功
 *
 *   【场景二：无已保存凭据（首次使用或凭据被清除）】
 *     上电 → load_wifi_credentials() 失败 → 启动 SmartConfig →
 *     等待手机 App（ESP Touch）发送 SSID/密码 →
 *     SC_EVENT_GOT_SSID_PSWD → save_wifi_credentials() →
 *     设置 STA 配置 → esp_wifi_connect() → ...
 *
 *   【场景三：Wi-Fi 断连】
 *     WIFI_EVENT_STA_DISCONNECTED → 重试（最多 5 次）→
 *     ──成功→ IP_EVENT_STA_GOT_IP → 恢复正常
 *     ──失败→ 启动 SmartConfig → 60s 超时 → esp_restart()
 *
 *   【场景四：SmartConfig 超时】
 *     SmartConfig 启动后 60 秒内未收到配网信息 →
 *     sc_timer_callback() → esp_restart() → 设备重启 →
 *     （如果之前保存过凭据）直接连接 → 恢复正常
 *
 * 【重要：SmartConfig 超时会重启设备】
 *   SmartConfig 在 60 秒内未收到配网数据就调用 esp_restart() 重启。
 *   重启后 LED 灯光状态由 led_control 的 NVS 持久化机制恢复，
 *   因此用户不会感知到灯光中断（只要有已保存凭据且 Wi-Fi 可连）。
 */

#include "wifi_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_wifi.h"
#include "esp_smartconfig.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "wifi_manager";

/** @brief Wi-Fi 连接状态事件组（供其他模块同步等待） */
static EventGroupHandle_t s_wifi_event_group;

/** @brief SmartConfig 超时定时器（60 秒，超时后重启设备） */
static TimerHandle_t sc_timer = NULL;

/** @brief SmartConfig 超时时间（秒） */
#define SC_TIMEOUT_SECONDS 60

/*===========================================================================
 * Wi-Fi 凭据的 NVS 持久化
 *===========================================================================*/

/**
 * @brief  将 Wi-Fi SSID 和密码保存到 NVS
 *
 * 保存路径：NVS 命名空间 "wifi_cred" → 键 "ssid" / "password"
 *
 * 调用时机：
 *   - SmartConfig 成功获取到配网信息后（SC_EVENT_GOT_SSID_PSWD）
 *
 * 保存后，下次启动时 load_wifi_credentials() 可以读取到这些凭据，
 * 从而跳过 SmartConfig 直接连接。
 *
 * @param ssid     Wi-Fi SSID 字符串（最长 32 字节，NUL 结尾）
 * @param password Wi-Fi 密码字符串（最长 64 字节，NUL 结尾）
 */
static void save_wifi_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_cred", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return;
    }
    nvs_set_str(nvs_handle, "ssid", ssid);
    nvs_set_str(nvs_handle, "password", password);
    nvs_commit(nvs_handle);  // 立即写入 Flash，防止断电丢失
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Wi-Fi credentials saved to NVS");
}

/**
 * @brief  从 NVS 读取已保存的 Wi-Fi 凭据
 *
 * 读取路径：NVS 命名空间 "wifi_cred" → 键 "ssid" / "password"
 *
 * @param ssid      输出缓冲区（接收 SSID 字符串）
 * @param ssid_len  缓冲区大小
 * @param password  输出缓冲区（接收密码字符串）
 * @param pass_len  缓冲区大小
 *
 * @return true  读取成功，凭据已写入 ssid/password 缓冲区
 * @return false NVS 中无凭据或读取失败
 */
static bool load_wifi_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_cred", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) return false;

    size_t ssid_size = ssid_len;
    err = nvs_get_str(nvs_handle, "ssid", ssid, &ssid_size);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return false;
    }

    size_t pass_size = pass_len;
    err = nvs_get_str(nvs_handle, "password", password, &pass_size);
    nvs_close(nvs_handle);
    return (err == ESP_OK);
}

/*===========================================================================
 * SmartConfig
 *===========================================================================*/

/**
 * @brief  SmartConfig 超时回调 — 重启设备
 *
 * 如果 SmartConfig 启动后 60 秒内未收到配网数据，
 * 说明用户可能不在现场或环境有问题，直接重启设备。
 *
 * 重启后的行为取决于 NVS 中是否有已保存的凭据：
 *   - 如有凭据 → 直接连接（上次配网成功过，只是偶发断连）
 *   - 如无凭据 → 再次进入 SmartConfig 等待配网
 *
 * 重启本身也会触发 led_control 从 NVS 恢复灯光状态，
 * 因此对 Home Assistant 端的影响可控。
 *
 * @param xTimer 触发此回调的定时器句柄
 */
static void sc_timer_callback(TimerHandle_t xTimer)
{
    ESP_LOGE(TAG, "SmartConfig timeout, restarting...");
    esp_restart();
}

/**
 * @brief  SmartConfig 监控任务
 *
 * 等待 SmartConfig 成功事件（BIT2），收到后停止超时定时器。
 * SmartConfig 成功后 Wi-Fi 密码已保存，后续由 wifi_event_handler
 * 接管连接流程。
 */
static void smartconfig_task(void *pvParameters)
{
    // 等待 SmartConfig 成功标志（BIT2）
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, BIT2,
                                           pdTRUE, pdFALSE, portMAX_DELAY);
    if (bits & BIT2) {
        // 停止超时定时器（配网已成功，不需要重启）
        if (sc_timer) xTimerStop(sc_timer, portMAX_DELAY);
        ESP_LOGI(TAG, "SmartConfig succeeded, waiting for Wi-Fi connection...");
    }
    vTaskDelete(NULL);
}

/*===========================================================================
 * Wi-Fi 事件处理器
 *===========================================================================*/

/**
 * @brief  Wi-Fi / IP / SmartConfig 综合事件回调
 *
 * 这是整个 Wi-Fi 模块的核心，统一处理三类事件：
 *   - WIFI_EVENT  : STA 启动、断连等
 *   - IP_EVENT    : DHCP 获取到 IP 地址
 *   - SC_EVENT    : SmartConfig 收到配网数据
 *
 * 事件处理逻辑：
 *
 *   WIFI_EVENT_STA_START:
 *     Wi-Fi 驱动启动完成 → 立即调用 esp_wifi_connect() 发起连接
 *
 *   WIFI_EVENT_STA_DISCONNECTED:
 *     断连处理分两步：
 *       1. retry_count < 5: 立即重连（可能只是信号波动）
 *       2. retry_count >= 5: 放弃重连，启动 SmartConfig 配网
 *     SmartConfig 会等待 60 秒，超时则重启设备。
 *     这保证了：短暂断网自动恢复，长期断网尝试重新配网。
 *
 *   IP_EVENT_STA_GOT_IP:
 *     DHCP 成功，Wi-Fi 连接真正可用。
 *     设置 WIFI_CONNECTED_BIT 通知等待中的 wifi_manager_init() 继续执行。
 *
 *   SC_EVENT_GOT_SSID_PSWD:
 *     SmartConfig 成功收到手机发送的 SSID 和密码。
 *     1. 保存凭据到 NVS 供下次启动使用
 *     2. 停止 SmartConfig 进程
 *     3. 用新凭据连接到 Wi-Fi
 *     4. 设置 BIT2 通知 smartconfig_task 停止超时定时器
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    static int retry_count = 0;  // 断连重试计数器（静态变量，跨事件调用保持）

    /*----------------------------------------------------------------------
     * Wi-Fi STA 启动完成 → 发起连接
     *--------------------------------------------------------------------*/
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }

    /*----------------------------------------------------------------------
     * Wi-Fi 断连处理
     *
     * 重试策略：最多 5 次快速重连，超过则切换到 SmartConfig。
     * 这种策略兼顾了"偶发断连快速恢复"和"AP 消失重新配网"两种场景。
     *--------------------------------------------------------------------*/
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count < 5) {
            esp_wifi_connect();
            retry_count++;
            ESP_LOGI(TAG, "Retry to connect to Wi-Fi (%d/5)", retry_count);
        } else {
            // 5 次重连都失败，启动 SmartConfig 重新配网
            ESP_LOGI(TAG, "Wi-Fi connection failed, starting SmartConfig");
            retry_count = 0;
            esp_wifi_disconnect();
            esp_wifi_restore();  // 清除之前失败的 Wi-Fi 配置
            smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
            ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));
            if (sc_timer) xTimerStart(sc_timer, portMAX_DELAY);
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "Wi-Fi disconnected");
    }

    /*----------------------------------------------------------------------
     * 获取到 IP 地址 — Wi-Fi 连接成功
     *
     * 此时 TCP/IP 协议栈已完全就绪，MQTT 客户端可以开始连接。
     * 设置 WIFI_CONNECTED_BIT 让 wifi_manager_init() 返回。
     *--------------------------------------------------------------------*/
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        retry_count = 0;  // 重置重试计数
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }

    /*----------------------------------------------------------------------
     * SmartConfig 接收到配网数据 — 最重要的配网事件
     *
     * SmartConfig 协议通过嗅探特定格式的 UDP 广播包来获取 SSID 和密码。
     * 手机 App（ESP Touch / 乐鑫 Esptouch）发送加密的 UDP 包，
     * ESP32 在混杂模式下捕获并解码。
     *
     * 收到凭据后的步骤：
     *   1. 保存到 NVS（下次启动直接用）
     *   2. 停止 SmartConfig（释放资源）
     *   3. 用新凭据配置并连接 Wi-Fi
     *   4. 通知 smartconfig_task 停止超时定时器（BIT2）
     *--------------------------------------------------------------------*/
    else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
        ESP_LOGI(TAG, "SmartConfig got SSID and password");
        smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;

        // SmartConfig 的事件数据可能不以 '\0' 结尾，安全拷贝
        char ssid[33] = {0};
        char password[65] = {0};
        strncpy(ssid, (char *)evt->ssid, sizeof(ssid) - 1);
        strncpy(password, (char *)evt->password, sizeof(password) - 1);
        ESP_LOGI(TAG, "SSID: %s, Password: %s", ssid, password);

        // 持久化：保存 Wi-Fi 凭据到 NVS
        save_wifi_credentials(ssid, password);

        // 停止 SmartConfig 进程（释放混杂模式资源）
        esp_smartconfig_stop();

        // 使用新凭据连接 Wi-Fi
        wifi_config_t wifi_config = {
            .sta = {
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,  // 最低安全级别
            },
        };
        strcpy((char *)wifi_config.sta.ssid, ssid);
        strcpy((char *)wifi_config.sta.password, password);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_connect());

        // 通知 smartconfig_task：配网成功，停止超时定时器
        xEventGroupSetBits(s_wifi_event_group, BIT2);
    }
}

/*===========================================================================
 * 模块初始化
 *===========================================================================*/

/**
 * @brief  初始化 Wi-Fi 管理器（阻塞等待，直到连接成功或失败）
 *
 * 初始化流程：
 *   1. 创建 EventGroup（用于同步和通知）
 *   2. 初始化 TCP/IP 协议栈和 Wi-Fi STA 接口
 *   3. 注册三类事件回调（Wi-Fi / IP / SmartConfig）
 *   4. 创建 SmartConfig 超时定时器（60 秒）
 *   5. 分支处理：
 *        - NVS 有凭据 → 直接用凭据配置并启动 Wi-Fi
 *        - NVS 无凭据 → 启动 SmartConfig 等待手机配网
 *   6. 创建 SmartConfig 监控任务
 *   7. 阻塞等待 WIFI_CONNECTED_BIT 或 WIFI_FAIL_BIT
 *
 * @note  这是一个阻塞调用，会一直等待直到 Wi-Fi 连接成功返回。
 *        如果一直连不上（无凭据 + SmartConfig 超时重启），
 *        调用方永远收不到返回。这是有意设计的固件行为。
 */
void wifi_manager_init(void)
{
    // 1. 创建事件组
    s_wifi_event_group = xEventGroupCreate();

    // 2. 初始化网络协议栈
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();  // 创建默认 Wi-Fi STA 接口

    // 3.1 初始化 Wi-Fi 驱动（使用默认配置）
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 3.2 注册所有事件回调
    esp_event_handler_instance_t instance_any_id, instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(SC_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));

    // 4. 创建 SmartConfig 超时定时器（60 秒，单次触发）
    sc_timer = xTimerCreate("sc_timer", pdMS_TO_TICKS(SC_TIMEOUT_SECONDS * 1000),
                            pdFALSE, NULL, sc_timer_callback);

    /*----------------------------------------------------------------------
     * 5. 分支：有凭据直连 / 无凭据启动 SmartConfig
     *
     * 优先使用 NVS 中保存的凭据，避免不必要的 SmartConfig 等待。
     * SmartConfig 需要在手机端操作，用户体验相对较差，
     * 仅在首次使用或 Wi-Fi 变更时才需要。
     *--------------------------------------------------------------------*/
    char saved_ssid[33] = {0}, saved_password[65] = {0};
    if (load_wifi_credentials(saved_ssid, sizeof(saved_ssid), saved_password, sizeof(saved_password))) {
        /*---- 分支 A: 有已保存凭据，直接连接 ----*/
        ESP_LOGI(TAG, "Found saved Wi-Fi credentials, connecting to SSID=%s", saved_ssid);

        wifi_config_t wifi_config = {
            .sta = {
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            },
        };
        strcpy((char *)wifi_config.sta.ssid, saved_ssid);
        strcpy((char *)wifi_config.sta.password, saved_password);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
    } else {
        /*---- 分支 B: 无凭据，启动 SmartConfig 配网 ----*/
        ESP_LOGI(TAG, "No saved Wi-Fi credentials, starting SmartConfig mode");

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());

        smartconfig_start_config_t sc_cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_smartconfig_start(&sc_cfg));

        // 启动超时定时器：60 秒内未收到配网数据则重启
        if (sc_timer) xTimerStart(sc_timer, portMAX_DELAY);
    }

    // 6. 创建 SmartConfig 监控任务（负责在配网成功后停止定时器）
    xTaskCreate(smartconfig_task, "sc_task", 4096, NULL, 5, NULL);

    /*----------------------------------------------------------------------
     * 7. 阻塞等待 Wi-Fi 连接成功
     *
     * portMAX_DELAY = 无限等待，直到 WIFI_CONNECTED_BIT 被设置。
     * 这意味着 app_main() 会在 wifi_manager_init() 调用处阻塞，
     * mqtt_ha_init() 和 rssi_reporter_start() 都要等 Wi-Fi 就绪后才执行。
     *--------------------------------------------------------------------*/
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi connected successfully");
    } else {
        ESP_LOGE(TAG, "Wi-Fi connection failed");
    }
}