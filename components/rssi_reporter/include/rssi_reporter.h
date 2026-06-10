/**
 * @file    rssi_reporter.h
 * @brief   Wi-Fi RSSI 信号强度定时上报模块
 *
 * 本模块创建独立的 FreeRTOS 任务，定时采集 Wi-Fi 信号强度（RSSI）
 * 并通过 MQTT 发布到 Home Assistant。
 *
 * Home Assistant 中对应实体：
 *   - 类型：sensor
 *   - 名称：Wi-Fi RSSI
 *   - 单位：dBm（分贝毫瓦）
 *   - 类别：diagnostic（诊断信息，默认不在仪表盘显示）
 *
 * 【调度策略】
 *   - 采集间隔：30 秒（固定的，避免频繁 Flash 写入和网络压力）
 *   - 任务栈大小：2048 字节
 *   - 任务优先级：5（FreeRTOS 标准优先级）
 */

#ifndef RSSI_REPORTER_H
#define RSSI_REPORTER_H

/**
 * @brief  启动 RSSI 定时上报任务
 *
 * 该函数立即返回，实际采集和上报在一个独立的后台任务中执行。
 *
 * 任务行为：
 *   - 无限循环，每 30 秒执行一次
 *   - 调用 esp_wifi_sta_get_ap_info() 获取当前连接的 AP 的 RSSI
 *   - 通过 mqtt_ha_publish() 发布到 rssi_state_topic
 *   - 使用 retain = 1 确保 HA 能随时获取最新值
 *
 * @note  必须在 Wi-Fi 已连接、MQTT 已初始化后调用
 */
void rssi_reporter_start(void);

#endif