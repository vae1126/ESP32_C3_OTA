/**
 * @file    device_id.c
 * @brief   设备标识与 MQTT 主题生成 — 实现
 *
 * 基于 MAC 地址生成唯一设备 ID 和所有 MQTT 主题字符串，
 * 写入全局变量供其他模块（mqtt_ha、rssi_reporter）使用。
 *
 * 【为什么用 MAC 地址】
 *   ESP32 的 Wi-Fi STA MAC 地址在出厂时烧录在 eFuse 中，全球唯一。
 *   用 MAC 作为设备 ID 保证同一 MQTT Broker 下多个设备不会冲突，
 *   也无需用户额外配置。
 *
 * 【主题命名规范】
 *   遵循 Home Assistant MQTT Discovery 的标准格式：
 *   <discovery_prefix>/<component>/<object_id>/<type>
 *
 *   其中 object_id 由 device_id + 后缀组成：
 *     - "{device_id}_pwm"     → PWM 灯的 object_id
 *     - "{device_id}_ws2812"  → WS2812 灯带的 object_id
 *     - "{device_id}"         → RSSI 传感器的 object_id（直接用 device_id）
 */

#include "device_id.h"
#include "esp_mac.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "device_id";

/*===========================================================================
 * 全局变量 — 设备标识
 *===========================================================================*/

/** @brief 基于 MAC 的唯一设备标识（如 "aabbccddeeff"） */
char base_device_id[DEVICE_ID_LEN] = {0};

/** @brief 设备显示名称 */
char base_device_name[DEVICE_NAME_LEN] = {0};

/** @brief 设备可用性主题：homeassistant/device/<id>/availability */
char device_avail_topic[TOPIC_LEN] = {0};

/*===========================================================================
 * 全局变量 — PWM RGB 灯主题
 *===========================================================================*/

char pwm_config_topic[TOPIC_LEN]  = {0};   // 发现配置主题
char pwm_command_topic[TOPIC_LEN] = {0};   // 命令接收主题
char pwm_state_topic[TOPIC_LEN]   = {0};   // 状态上报主题

/*===========================================================================
 * 全局变量 — WS2812 灯带主题
 *===========================================================================*/

char ws2812_config_topic[TOPIC_LEN]  = {0};
char ws2812_command_topic[TOPIC_LEN] = {0};
char ws2812_state_topic[TOPIC_LEN]   = {0};

/*===========================================================================
 * 全局变量 — RSSI 传感器主题
 *===========================================================================*/

char rssi_config_topic[TOPIC_LEN] = {0};   // 发现配置主题
char rssi_state_topic[TOPIC_LEN]  = {0};   // 状态上报主题

/*===========================================================================
 * 全局变量 — OTA 升级主题（预留）
 *===========================================================================*/

char ota_command_topic[TOPIC_LEN]       = {0};
char ota_button_config_topic[TOPIC_LEN] = {0};
char ota_progress_topic[TOPIC_LEN]      = {0};

/*===========================================================================
 * device_id_init — 生成设备 ID 和所有 MQTT 主题
 *===========================================================================*/

/**
 * @brief  读取 MAC 地址并生成设备 ID 和所有 MQTT 主题
 *
 * 生成的设备 ID 格式：12 位小写十六进制 MAC 地址
 *   MAC = AA:BB:CC:DD:EE:FF → device_id = "aabbccddeeff"
 *
 * MQTT 主题格式遵循 Home Assistant 默认发现规范：
 *   homeassistant/<component>/<object_id>/<type>
 *
 * 主题示例（假设 device_id = aabbccddeeff）：
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ 组件          │ Topic                                   │ 用途  │
 *   ├──────────────────────────────────────────────────────────────┤
 *   │ device_avail   │ homeassistant/device/aabbccddeeff/avail  │ 在线  │
 *   │ pwm_config     │ homeassistant/light/aabbccddeeff_pwm/cfg │ 发现  │
 *   │ pwm_command    │ homeassistant/light/aabbccddeeff_pwm/set │ 命令  │
 *   │ pwm_state      │ homeassistant/light/aabbccddeeff_pwm/st  │ 状态  │
 *   │ ws2812_config  │ homeassistant/light/aabbccddeeff_ws/cfg  │ 发现  │
 *   │ ws2812_command │ homeassistant/light/aabbccddeeff_ws/set  │ 命令  │
 *   │ ws2812_state   │ homeassistant/light/aabbccddeeff_ws/st   │ 状态  │
 *   │ rssi_config    │ homeassistant/sensor/aabbccddeeff/rssi/cfg│ 发现  │
 *   │ rssi_state     │ homeassistant/sensor/aabbccddeeff/rssi/st │ 状态  │
 *   └──────────────────────────────────────────────────────────────┘
 */
void device_id_init(void)
{
    /*----------------------------------------------------------------------
     * 1. 读取 Wi-Fi STA 接口的 MAC 地址
     *
     * esp_read_mac(mac, ESP_MAC_WIFI_STA) 读取的是 Wi-Fi Station 接口
     * 的 MAC 地址。这个地址在出厂时写入 eFuse，除非用户手动修改，
     * 否则在整个芯片生命周期内保持不变且全球唯一。
     *--------------------------------------------------------------------*/
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    // 格式化为 12 位小写十六进制字符串
    char mac_str[13];
    snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /*----------------------------------------------------------------------
     * 2. 设置设备标识
     *
     * - base_device_id:   纯 MAC 字符串，用作 unique_id 和主题中的 object_id
     * - base_device_name: 包含 MAC 后缀的人类可读名称，方便用户在 HA 中识别
     *--------------------------------------------------------------------*/
    snprintf(base_device_id, sizeof(base_device_id), "%s", mac_str);
    snprintf(base_device_name, sizeof(base_device_name), "ESP32 Light (%s)", mac_str);

    /*----------------------------------------------------------------------
     * 3. 生成 device 可用性主题
     *
     * 这是所有实体的共同 availability topic。设备在线时发布 "online"，
     * 设备离线时（LWT 遗嘱消息）Broker 自动发布 "offline"。
     * Home Assistant 通过监听此主题管理所有关联实体的"可用/不可用"状态。
     *--------------------------------------------------------------------*/
    snprintf(device_avail_topic, sizeof(device_avail_topic),
             "homeassistant/device/%s/availability", base_device_id);

    /*----------------------------------------------------------------------
     * 4. 生成 PWM RGB 灯的三个主题
     *
     * object_id = "{device_id}_pwm"，确保与其他实体不冲突
     *--------------------------------------------------------------------*/
    snprintf(pwm_config_topic, sizeof(pwm_config_topic),
             "homeassistant/light/%s_pwm/config", base_device_id);
    snprintf(pwm_command_topic, sizeof(pwm_command_topic),
             "homeassistant/light/%s_pwm/set", base_device_id);
    snprintf(pwm_state_topic, sizeof(pwm_state_topic),
             "homeassistant/light/%s_pwm/state", base_device_id);

    /*----------------------------------------------------------------------
     * 5. 生成 WS2812 灯带的三个主题
     *
     * object_id = "{device_id}_ws2812"
     *--------------------------------------------------------------------*/
    snprintf(ws2812_config_topic, sizeof(ws2812_config_topic),
             "homeassistant/light/%s_ws2812/config", base_device_id);
    snprintf(ws2812_command_topic, sizeof(ws2812_command_topic),
             "homeassistant/light/%s_ws2812/set", base_device_id);
    snprintf(ws2812_state_topic, sizeof(ws2812_state_topic),
             "homeassistant/light/%s_ws2812/state", base_device_id);

    /*----------------------------------------------------------------------
     * 6. 生成 RSSI 传感器的两个主题
     *
     * object_id = "{device_id}"（直接用 device_id，因为 sensor 组件
     * 不会与 light 冲突）
     *--------------------------------------------------------------------*/
    snprintf(rssi_config_topic, sizeof(rssi_config_topic),
             "homeassistant/sensor/%s/rssi/config", base_device_id);
    snprintf(rssi_state_topic, sizeof(rssi_state_topic),
             "homeassistant/sensor/%s/rssi/state", base_device_id);

    /*----------------------------------------------------------------------
     * 7. 生成 OTA 升级主题
     *
     * command   : 接收服务器推送的升级/降级命令
     * config    : Home Assistant 按钮实体发现（可在 HA 中触发检查）
     * progress  : 升级进度百分比和状态上报
     *--------------------------------------------------------------------*/
    snprintf(ota_command_topic, sizeof(ota_command_topic),
             "homeassistant/ota/%s/command", base_device_id);
    snprintf(ota_button_config_topic, sizeof(ota_button_config_topic),
             "homeassistant/button/%s_ota/config", base_device_id);
    snprintf(ota_progress_topic, sizeof(ota_progress_topic),
             "homeassistant/ota/%s/progress", base_device_id);

    ESP_LOGI(TAG, "Device ID: %s", base_device_id);
}