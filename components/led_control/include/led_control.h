/**
 * @file    led_control.h
 * @brief   双路 RGB LED 控制模块（PWM + WS2812）
 *
 * 本模块提供两路独立灯光的控制接口：
 *   - PWM RGB LED    : 三通道 LEDC 驱动的通用 RGB LED（共阳/共阴灯珠）
 *   - WS2812 LED 灯带 : RMT 接口驱动的可寻址 RGB LED 灯带（支持多颗串联）
 *
 * 【NVS 断电保持】
 *   每次调用 set_state() 修改灯光状态后，自动将状态写入 NVS 持久化。
 *   下一次启动时，led_control_init() 会从 NVS 自动恢复上次的灯光效果。
 *   这意味着断电、重启、或 Wi-Fi 断连导致的重启都不会丢失灯光状态。
 *
 * 依赖：
 *   - nvs_flash : 状态持久化存储
 *   - driver/ledc : PWM 驱动
 *   - led_strip : WS2812 RMT 驱动
 */

#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief LED 状态结构体
 *
 * 包含灯光的所有可控属性：
 *   - state      : 开关状态（true=开，false=关）
 *   - brightness : 整体亮度（0-255，由 Home Assistant 传入）
 *   - red/green/blue : RGB 颜色分量（0-255）
 *
 * 实际硬件占空比由 set_state 函数根据公式计算：
 *   duty = state ? (color × brightness) / 255 : 0
 */
typedef struct {
    bool state;
    uint8_t brightness;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} led_state_t;

/**
 * @brief  初始化灯光控制模块（含 NVS 状态恢复）
 *
 * 启动流程：
 *   1. 初始化 LEDC PWM 硬件（三通道，频率 5 kHz，8 位分辨率）
 *   2. 初始化 WS2812 RMT 硬件
 *   3. 从 NVS 读取上次保存的灯光状态并应用到硬件
 *      - 首次启动（无保存数据）或数据损坏时，默认保持关闭状态
 *      - 存在保存数据时，自动恢复到断电前的亮度和颜色
 *
 * @note  必须在 nvs_flash_init() 之后调用
 */
void led_control_init(void);

/**
 * @brief  设置 PWM RGB 灯状态并自动保存到 NVS
 *
 * 每次调用会：
 *   1. 写入 LEDC 三通道 PWM 占空比
 *   2. 自动保存新状态到 NVS（断电可恢复）
 *
 * @param state  目标状态，为 NULL 时安全返回
 */
void led_pwm_set_state(const led_state_t *state);

/**
 * @brief  设置 WS2812 灯带状态并自动保存到 NVS
 *
 * 每次调用会：
 *   1. 刷新所有 WS2812 像素
 *   2. 自动保存新状态到 NVS（断电可恢复）
 *
 * @param state  目标状态，为 NULL 或硬件未初始化时安全返回
 */
void led_ws2812_set_state(const led_state_t *state);

/**
 * @brief  获取 PWM 灯当前状态（RAM 副本）
 * @return 指向内部 pwm_state 的指针，调用方不应释放
 */
led_state_t *led_pwm_get_state(void);

/**
 * @brief  获取 WS2812 灯带当前状态（RAM 副本）
 * @return 指向内部 ws2812_state 的指针，调用方不应释放
 */
led_state_t *led_ws2812_get_state(void);

#endif