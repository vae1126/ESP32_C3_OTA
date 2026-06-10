/**
 * @file    wifi_manager.h
 * @brief   Wi-Fi 连接管理器
 *
 * 本模块负责 ESP32 Wi-Fi Station 模式的生命周期管理：
 *   1. 自动连接已保存的 Wi-Fi（凭据存储在 NVS）
 *   2. 无已保存凭据时启动 SmartConfig（一键配网）
 *   3. Wi-Fi 断连后自动重试（最多 5 次）
 *   4. 通过 EventGroup 向其他模块通知 Wi-Fi 连接状态
 *
 * 【SmartConfig 说明】
 *   SmartConfig 是乐鑫提供的 Wi-Fi 配网技术，用户通过手机 App
 *   （如 ESP Touch）将 SSID 和密码发送给 ESP32，无需在固件中硬编码。
 *   SmartConfig 成功后凭据自动保存到 NVS，下次启动直接连接。
 *
 * 【配网流程】
 *   上电 → NVS 有凭据? ──是→ 直接连接 → 成功 → 就绪
 *                      ──否→ SmartConfig → 手机配网 → 保存凭据 → 连接 → 就绪
 *
 * 【依赖】
 *   - nvs_flash : 存储 Wi-Fi 凭据
 *   - esp_wifi   : ESP-IDF Wi-Fi 驱动
 *   - esp_smartconfig : SmartConfig 配网协议
 *   - FreeRTOS   : EventGroup、Timer、Task
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_event.h"

/**
 * @brief 事件组标志位：Wi-Fi 连接成功
 *
 * wifi_manager_init() 会在 Wi-Fi 成功获取 IP 地址后设置此标志位。
 * 调用方可使用 xEventGroupWaitBits() 等待此标志位，实现同步。
 */
#define WIFI_CONNECTED_BIT BIT0

/**
 * @brief 事件组标志位：Wi-Fi 连接失败（预留）
 *
 * 当前版本如果 NVS 有凭据但连接失败，会进入 SmartConfig 模式，
 * 因此 WIFI_FAIL_BIT 暂未使用，保留供后续扩展。
 */
#define WIFI_FAIL_BIT      BIT1

/**
 * @brief  初始化 Wi-Fi 管理器（阻塞等待，直到连接成功或失败）
 *
 * 该函数是阻塞调用，内部会：
 *   1. 创建 EventGroup（用于与其他模块同步连接状态）
 *   2. 初始化 TCP/IP 协议栈和 Wi-Fi Station 接口
 *   3. 注册 Wi-Fi / IP / SmartConfig 事件回调
 *   4. 尝试用 NVS 中保存的凭据连接 Wi-Fi
 *   5. 无凭据时启动 SmartConfig 等待手机配网
 *   6. 阻塞等待连接成功标志位
 *
 * @note  必须在 nvs_flash_init() 之后调用（Wi-Fi 凭据存储在 NVS）
 */
void wifi_manager_init(void);

/**
 * @brief  获取 Wi-Fi 事件组句柄（供其他模块查询连接状态）
 *
 * 使用方式：
 *   EventBits_t bits = xEventGroupWaitBits(
 *       wifi_manager_get_event_group(),
 *       WIFI_CONNECTED_BIT,
 *       pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
 *   if (bits & WIFI_CONNECTED_BIT) { ... }
 *
 * @return FreeRTOS EventGroup 句柄
 */
EventGroupHandle_t wifi_manager_get_event_group(void);

#endif