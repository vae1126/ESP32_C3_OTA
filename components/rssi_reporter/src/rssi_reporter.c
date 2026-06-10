/**
 * @file    rssi_reporter.c
 * @brief   Wi-Fi RSSI 信号强度定时上报 — 实现
 *
 * 本模块创建一个长时间运行的 FreeRTOS 后台任务，每 30 秒：
 *   1. 调用 esp_wifi_sta_get_ap_info() 获取当前连接 AP 的 RSSI 值
 *   2. 将 RSSI 格式化为字符串（如 "-45"）
 *   3. 通过 MQTT 发布到 Home Assistant 的 rssi_state_topic
 *
 * 【RSSI 数值含义（经验值）】
 *   -30 dBm ~ 0 dBm : 极强信号（设备紧邻 AP）
 *   -50 dBm ~ -30 dBm: 强信号，稳定可靠
 *   -70 dBm ~ -50 dBm: 中等信号，仍可正常工作
 *   -80 dBm ~ -70 dBm: 弱信号，可能出现丢包
 *   -90 dBm ~ -80 dBm: 极弱信号，连接不稳定
 *   低于 -90 dBm      : 基本无法通信
 *
 * 【为什么每 30 秒上报一次】
 *   - RSSI 是慢变化指标，不需要高频采样
 *   - 降低 MQTT 发布开销和 Flash 写入频率
 *   - Home Assistant 传感器的默认更新间隔足够
 *
 * 【依赖】
 *   - device_id  : 提供 rssi_state_topic（MQTT 主题名称）
 *   - mqtt_ha    : 提供 mqtt_ha_publish() 发布接口
 *   - esp_wifi   : 提供 esp_wifi_sta_get_ap_info() 读取 RSSI
 */

#include "rssi_reporter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "device_id.h"
#include "mqtt_ha.h"
#include <stdio.h>

static const char *TAG = "rssi_reporter";

/**
 * @brief  获取当前连接的 Wi-Fi AP 的信号强度
 *
 * 通过 esp_wifi_sta_get_ap_info() 读取 AP 的 RSSI。
 * 该函数从 Wi-Fi 驱动的内部状态中读取，不产生额外的无线通信开销。
 *
 * @return RSSI 值（dBm），如果未连接到 AP 则返回 0
 */
static int8_t get_wifi_rssi(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;  // 负值，单位 dBm
    }
    return 0;
}

/**
 * @brief  RSSI 定时上报任务（FreeRTOS 任务入口）
 *
 * 任务生命周期：创建后无限循环运行，伴随整个固件生命周期。
 *
 * 每轮循环：
 *   1. 等待 30 秒（vTaskDelay，让出 CPU 给其他任务）
 *   2. 获取当前 RSSI 值
 *   3. 格式化为字符串（如 "-45"）
 *   4. 发布到 MQTT（retain=true，新订阅者立即获取最新值）
 *   5. 输出日志（INFO 级别，方便调试）
 *
 * 错误处理：
 *   - 如果 Wi-Fi 未连接，get_wifi_rssi() 返回 0，发布 "0" 给 HA
 *   - 如果 MQTT 未连接，mqtt_ha_publish() 安全返回（内部有空指针检查）
 *
 * @param pvParameters 任务参数（未使用，传 NULL）
 */
static void rssi_task(void *pvParameters)
{
    char rssi_str[8];  // RSSI 字符串缓冲区（如 "-100\0" 只需 5 字节，预留安全空间）

    while (1) {
        // 等待 30 秒，期间让出 CPU
        vTaskDelay(pdMS_TO_TICKS(30000));

        // 读取 Wi-Fi RSSI
        int8_t rssi = get_wifi_rssi();

        // 格式化为字符串并发布
        snprintf(rssi_str, sizeof(rssi_str), "%d", rssi);
        mqtt_ha_publish(rssi_state_topic, rssi_str, 1);  // retain=1，持久化最新值

        ESP_LOGI(TAG, "Published RSSI: %d dBm", rssi);
    }

    // 理论上不会执行到这里，但保留析构逻辑以符合 FreeRTOS 规范
    vTaskDelete(NULL);
}

/**
 * @brief  启动 RSSI 上报后台任务
 *
 * 使用 xTaskCreate() 创建一个 FreeRTOS 任务。
 * 任务参数说明：
 *   - 栈大小: 2048 字节（足够存放局部变量和调用栈）
 *   - 优先级: 5（中等，高于 IDLE 但低于 Wi-Fi/MQTT）
 *   - 句柄: 未保存（不需要从外部控制该任务）
 *
 * @note  必须在 Wi-Fi 和 MQTT 均已初始化后调用
 */
void rssi_reporter_start(void)
{
    xTaskCreate(rssi_task, "rssi_task", 2048, NULL, 5, NULL);
}