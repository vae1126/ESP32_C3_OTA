/**
 * @file    app_main.c
 * @brief   ESP32 智能灯光控制器 — 主程序入口
 *
 * 本文件是固件的启动入口，负责按顺序初始化所有子系统。
 *
 * 【启动流程】
 *   app_main() 在 FreeRTOS 启动后由 ESP-IDF 自动调用，执行顺序：
 *     1. NVS Flash       — 非易失性存储（Wi-Fi 凭据、灯光状态等）
 *     2. device_id       — 基于 MAC 生成唯一设备 ID 和 MQTT 主题
 *     3. led_control     — 初始化 PWM + WS2812 硬件，从 NVS 恢复灯光状态
 *     4. wifi_manager    — 连接 Wi-Fi（已保存凭据直连 / SmartConfig 配网）
 *     5. mqtt_ha         — 连接 MQTT Broker，注册 Home Assistant 自动发现
 *     6. ota_handler     — 初始化 OTA 固件升级（定时检查 + MQTT 命令订阅）
 *     7. rssi_reporter   — 启动 RSSI 定时上报任务
 *
 * 【架构说明】
 *   采用模块化组件设计，各子功能封装在 components/ 目录下：
 *     - device_id        : 设备标识与 MQTT 主题生成
 *     - led_control      : 双路 LED 硬件驱动 + NVS 状态持久化
 *     - wifi_manager     : Wi-Fi 管理（含 SmartConfig 配网）
 *     - mqtt_ha          : MQTT + Home Assistant 集成
 *     - rssi_reporter    : Wi-Fi 信号强度定时上报
 *
 * 【故障恢复】
 *   - Wi-Fi 断连 → 自动重连（5 次）→ 失败后启动 SmartConfig
 *   - MQTT 断连 → 自动重连，重连后重新注册 HA 发现配置
 *   - 异常重启 → led_control 从 NVS 恢复灯光状态
 *   - SmartConfig 超时 → esp_restart() 重启设备
 *
 * 【依赖】
 *   ESP-IDF v5.x, ESP32-C3 或其他 ESP32 系列芯片
 */

#include "device_id.h"
#include "wifi_manager.h"
#include "led_control.h"
#include "mqtt_ha.h"
#include "ota_handler.h"
#include "rssi_reporter.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "main";

/**
 * @brief  应用程序主入口（FreeRTOS 启动后由 ESP-IDF 自动调用）
 *
 * 每个步骤的设计考量：
 *
 *   Step 1 - NVS 初始化
 *     NVS（Non-Volatile Storage）是 ESP32 Flash 上的键值存储，
 *     用于持久化 Wi-Fi 凭据、灯光状态等。如果 NVS 分区已满或版本升级，
 *     则先擦除再重新初始化（会丢失已保存的数据，但能保证系统正常运行）。
 *
 *   Step 2 - 设备 ID
 *     必须在其他模块之前初始化，因为 wifi_manager 和 mqtt_ha 都依赖
 *     device_id 提供的 MQTT 主题名称（如 command topic、state topic）。
 *
 *   Step 3 - LED 控制
 *     硬件初始化 + NVS 状态恢复。内部会：
 *       - 配置 LEDC PWM 三通道和 WS2812 RMT 接口
 *       - 从 NVS 读取上次保存的灯光状态并立即应用到硬件
 *     （首次启动时 NVS 无数据，默认关闭状态）
 *
 *   Step 4 - Wi-Fi 管理器
 *     这是一个阻塞调用：会等待 Wi-Fi 连接成功（或失败）后才返回。
 *     如果 NVS 中有已保存的 Wi-Fi 凭据，直接连接；
 *     否则进入 SmartConfig 模式等待手机配网。
 *
 *   Step 5 - MQTT + Home Assistant
 *     连接 MQTT Broker，注册 HA 自动发现实体。
 *     连接成功后自动：发布在线状态、发送发现配置、订阅命令主题。
 *
 *   Step 6 - RSSI 上报
 *     创建独立的 FreeRTOS 任务，每 30 秒采集一次 Wi-Fi RSSI
 *     并通过 MQTT 发布到 Home Assistant。
 */
void app_main(void)
{
    ESP_LOGI(TAG, "Starting application...");

    /*----------------------------------------------------------------------
     * Step 1: 初始化 NVS（非易失性存储）
     *
     * NVS 存储内容：
     *   - "wifi_cred" 空间 : SSID + 密码（wifi_manager 写入）
     *   - "led_state" 空间 : 灯光开关/亮度/颜色（led_control 写入）
     *
     * 错误处理：
     *   - ESP_ERR_NVS_NO_FREE_PAGES: 分区写满 → 擦除后重新初始化
     *   - ESP_ERR_NVS_NEW_VERSION_FOUND: 固件升级后 NVS 格式不兼容 → 擦除
     *   擦除会清空所有 NVS 数据（Wi-Fi 密码和灯光状态），但保证系统可启动
     *--------------------------------------------------------------------*/
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /*----------------------------------------------------------------------
     * Step 2-6: 按依赖顺序初始化各子系统
     *
     * 初始化顺序是严格按依赖关系排列的：
     *   device_id ← led_control ← wifi_manager ← mqtt_ha ← rssi_reporter
     *
     * 注意：wifi_manager_init() 是阻塞的，会等待 Wi-Fi 就绪后才返回。
     * 如果 Wi-Fi 一直连不上（无已保存凭据且未完成 SmartConfig），
     * 程序会永远卡在这里。这是有意为之：没有网络，后续 MQTT 也无意义。
     *--------------------------------------------------------------------*/
    device_id_init();       // Step 2: 生成设备 ID 和 MQTT 主题名称
    led_control_init();     // Step 3: 初始化 LED 硬件 + 从 NVS 恢复灯光状态
    wifi_manager_init();    // Step 4: 连接 Wi-Fi（阻塞等待）
    mqtt_ha_init();         // Step 5: 连接 MQTT Broker + HA 自动发现
    ota_handler_init();     // Step 6: 初始化 OTA 固件升级（定时检查+MQTT命令）
    rssi_reporter_start();  // Step 7: 启动 RSSI 定时上报任务

    ESP_LOGI(TAG, "System ready");
    ESP_LOGI(TAG, "Firmware version: %s", ota_get_version());
}