/**
 * @file    mqtt_ha.h
 * @brief   MQTT + Home Assistant 集成模块
 *
 * 本模块负责：
 *   1. 建立并维护与 MQTT Broker 的连接
 *   2. 实现 Home Assistant MQTT 自动发现协议（MQTT Discovery）
 *   3. 订阅并解析来自 HA 的灯光控制命令（JSON 格式）
 *   4. 发布设备状态（在线/离线、灯光属性、RSSI 等）
 *
 * 支持的 Home Assistant 实体：
 *   - Light (PWM RGB)   — 基于 LEDC 的三通道 PWM RGB 灯
 *   - Light (WS2812)     — 可寻址 RGB LED 灯带
 *   - Sensor (WiFi RSSI) — Wi-Fi 信号强度诊断传感器
 *
 * 依赖：
 *   - esp_mqtt_client（ESP-IDF 官方 MQTT 客户端库）
 *   - device_id（设备标识与 MQTT 主题生成）
 *   - led_control（灯光硬件控制）
 *   - cJSON（JSON 解析与构造）
 */

#ifndef MQTT_HA_H
#define MQTT_HA_H

#include "mqtt_client.h"

/**
 * @brief  初始化 MQTT 连接并启动 Home Assistant 集成
 *
 * 该函数完成以下工作：
 *   - 基于 Wi-Fi STA MAC 地址生成唯一的 MQTT Client ID
 *   - 通过 Kconfig（menuconfig）读取 Broker URI 等配置参数
 *   - 配置 LWT（Last Will and Testament）遗嘱消息：
 *       设备意外离线时，Broker 会自动向 availability 主题发布 "offline"
 *   - 注册 MQTT 事件回调（连接成功时发布发现配置和在线状态，
 *     接收到数据时解析灯光控制命令）
 *   - 启动 MQTT 客户端（内部自动管理重连）
 *
 * @note  必须在 Wi-Fi 已连接、device_id_init() 调用之后调用本函数
 */
void mqtt_ha_init(void);

/**
 * @brief  向 MQTT Broker 发布一条消息
 *
 * @param  topic   目标主题（topic），通常使用 device_id 模块生成的全局主题变量
 * @param  payload 消息体（字符串），如 JSON 格式的状态数据
 * @param  retain  是否保留消息（0 = 不保留，1 = 保留）
 *                 保留消息会在新订阅者上线时立即推送给它，适用于状态主题
 *
 * @note  如果 MQTT 客户端尚未初始化或未连接，本函数为安全空操作
 */
void mqtt_ha_publish(const char *topic, const char *payload, int retain);

/**
 * @brief  获取 MQTT 客户端句柄
 *
 * @return esp_mqtt_client_handle_t  MQTT 客户端句柄，
 *         如果尚未初始化则返回 NULL
 *
 * @note  其他模块（如 rssi_reporter）可使用该句柄直接调用
 *        esp_mqtt_client 的原生 API
 */
esp_mqtt_client_handle_t mqtt_ha_get_client(void);

#endif