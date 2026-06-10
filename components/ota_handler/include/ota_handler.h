/**
 * @file    ota_handler.h
 * @brief   OTA 固件升级模块 — 公开 API
 *
 * 本模块负责：
 *   1. 定时通过 HTTPS 检查服务器是否有新固件版本
 *   2. 通过 MQTT 接收升级/降级命令
 *   3. 使用 esp_https_ota API 下载并刷写固件
 *   4. 上报升级进度和结果
 *   5. 管理 ESP-IDF 内置的 OTA 回滚机制
 *
 * 依赖（按初始化顺序）：
 *   Wi-Fi 已连接 → device_id 已初始化 → MQTT 已连接
 *
 * 使用方式：
 *   在 app_main() 中，Wi-Fi + MQTT 就绪后调用 ota_handler_init()。
 */

#ifndef OTA_HANDLER_H
#define OTA_HANDLER_H

#include <stdint.h>

/**
 * @brief  OTA 升级进度回调函数类型
 *
 * @param progress_pct  下载/刷写进度 (0-100)
 * @param status_msg    状态文本描述，如 "Downloading..."、"Flashing..."、"Done"
 */
typedef void (*ota_progress_cb_t)(int progress_pct, const char *status_msg);

/**
 * @brief  初始化 OTA 模块
 *
 * 工作流程：
 *   1. 从 NVS 读取当前固件版本号
 *   2. 处理 OTA 回滚（如果上次升级后启动失败）
 *   3. 注册 OTA 进度上报到 MQTT progress topic
 *   4. 订阅 MQTT OTA 命令主题（接收升级/降级指令）
 *   5. 创建后台定时任务：每 CONFIG_OTA_CHECK_INTERVAL_MIN 分钟检查新版本
 *   6. 首次调用时立即执行一次版本检查
 *
 * 调用时机：
 *   - Wi-Fi 已连接（wifi_manager_init() 返回后）
 *   - MQTT 已连接（mqtt_ha_init() 返回后）
 *   - device_id_init() 已完成（OTA topic 已生成）
 */
void ota_handler_init(void);

/**
 * @brief  获取当前固件版本号
 *
 * 版本号来源：
 *   - 编译时宏 FIRMWARE_VERSION（由 CMake 注入）
 *   - 如果宏未定义，返回 "0.0.0"
 *
 * @return 版本号字符串（如 "3.0.1"），生命周期为静态内存，不可释放
 */
const char *ota_get_version(void);

/**
 * @brief  立即执行一次版本检查（跳过定时器等待）
 *
 * 可用于：
 *   - MQTT 收到 "check" 命令时立即触发
 *   - 设备刚上线时快速同步
 *
 * 检查结果：
 *   - 有新版本且自动升级开启 → 自动下载 + 刷写 + 重启
 *   - 有新版本但自动升级关闭 → 仅通过 MQTT 通知
 *   - 无新版本 → 仅记录日志
 */
void ota_check_now(void);

/**
 * @brief  注册 OTA 进度回调（可选）
 *
 * @param cb  回调函数指针，设为 NULL 则取消回调
 *
 * 典型用法：外部模块（如 LED 指示灯）通过此回调获知升级进度，
 * 从而提供用户可见的升级状态指示。
 */
void ota_register_progress_callback(ota_progress_cb_t cb);

#endif /* OTA_HANDLER_H */
