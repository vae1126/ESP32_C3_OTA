/**
 * @file    device_id.h
 * @brief   设备标识与 MQTT 主题生成模块
 *
 * 本模块负责：
 *   1. 基于 Wi-Fi STA MAC 地址生成全球唯一的设备 ID
 *   2. 根据设备 ID 生成所有 MQTT 主题（遵循 Home Assistant 命名规范）
 *   3. 通过全局变量向其他模块暴露设备标识和主题名称
 *
 * 【Home Assistant MQTT 主题规范】
 *   discovery_prefix / component / object_id / config     — 发现配置
 *   discovery_prefix / component / object_id / state      — 状态上报
 *   discovery_prefix / component / object_id / set        — 命令接收
 *   discovery_prefix / device   / device_id  / availability — 在线/离线
 *
 *   本项目 discovery_prefix = "homeassistant"（HA 默认值）
 *
 * 【全局变量说明】
 *   所有 Topic 变量均为 128 字节的字符数组，由 device_id_init() 赋值。
 *   其他模块（mqtt_ha、rssi_reporter）通过 extern 声明引用。
 *   建议在其他模块中只读使用，不要修改。
 */

#ifndef DEVICE_ID_H
#define DEVICE_ID_H

#include <stddef.h>

/** @brief 设备 ID 最大长度（12 位十六进制 MAC + '\0'） */
#define DEVICE_ID_LEN 13

/** @brief 设备名称最大长度 */
#define DEVICE_NAME_LEN 64

/** @brief MQTT 主题字符串最大长度 */
#define TOPIC_LEN 128

/*===========================================================================
 * 设备基本标识
 *===========================================================================*/

/** @brief 基于 MAC 地址的唯一设备 ID（如 "aabbccddeeff"） */
extern char base_device_id[DEVICE_ID_LEN];

/** @brief 设备显示名称（如 "ESP32 Light (aabbccddeeff)"） */
extern char base_device_name[DEVICE_NAME_LEN];

/** @brief 设备可用性主题（发布 online/offline，用于 HA 的 availability 机制） */
extern char device_avail_topic[TOPIC_LEN];

/*===========================================================================
 * PWM RGB 灯 — MQTT 主题
 *===========================================================================*/

/** @brief PWM 灯的 Home Assistant 发现配置主题 */
extern char pwm_config_topic[TOPIC_LEN];

/** @brief PWM 灯的命令接收主题（HA → 设备） */
extern char pwm_command_topic[TOPIC_LEN];

/** @brief PWM 灯的状态上报主题（设备 → HA） */
extern char pwm_state_topic[TOPIC_LEN];

/*===========================================================================
 * WS2812 灯带 — MQTT 主题
 *===========================================================================*/

/** @brief WS2812 灯带的 Home Assistant 发现配置主题 */
extern char ws2812_config_topic[TOPIC_LEN];

/** @brief WS2812 灯带的命令接收主题（HA → 设备） */
extern char ws2812_command_topic[TOPIC_LEN];

/** @brief WS2812 灯带的状态上报主题（设备 → HA） */
extern char ws2812_state_topic[TOPIC_LEN];

/*===========================================================================
 * Wi-Fi RSSI 传感器 — MQTT 主题
 *===========================================================================*/

/** @brief RSSI 传感器的 Home Assistant 发现配置主题 */
extern char rssi_config_topic[TOPIC_LEN];

/** @brief RSSI 传感器的状态上报主题（设备 → HA） */
extern char rssi_state_topic[TOPIC_LEN];

/*===========================================================================
 * OTA 升级 — MQTT 主题（预留，当前未使用）
 *===========================================================================*/

/** @brief OTA 升级命令主题（预留） */
extern char ota_command_topic[TOPIC_LEN];

/** @brief OTA 按钮的 Home Assistant 发现配置主题（预留） */
extern char ota_button_config_topic[TOPIC_LEN];

/** @brief OTA 升级进度主题（预留） */
extern char ota_progress_topic[TOPIC_LEN];

/*===========================================================================
 * API
 *===========================================================================*/

/**
 * @brief  初始化设备 ID 和所有 MQTT 主题
 *
 * 调用时机：在 nvs_flash_init() 之后、任何使用 Topic 的模块之前调用。
 *
 * 工作流程：
 *   1. 读取 Wi-Fi STA MAC 地址（烧录在 eFuse 中，芯片唯一）
 *   2. 格式化为 12 位小写十六进制字符串作为 device_id
 *   3. 生成所有 MQTT 主题名称并写入全局变量
 *
 * 主题命名示例（MAC = aabbccddeeff）：
 *   base_device_id       = "aabbccddeeff"
 *   pwm_command_topic    = "homeassistant/light/aabbccddeeff_pwm/set"
 *   pwm_state_topic      = "homeassistant/light/aabbccddeeff_pwm/state"
 *   device_avail_topic   = "homeassistant/device/aabbccddeeff/availability"
 */
void device_id_init(void);

#endif