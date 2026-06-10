/**
 * @file    led_control.c
 * @brief   LED 控制模块 — 双路灯光驱动（PWM + WS2812）
 *
 * 本模块管理两路独立的 RGB 灯光硬件：
 *   1. PWM RGB 灯   — 三通道 LEDC 直接驱动 R/G/B 引脚（如共阳/共阴 LED 模组）
 *   2. WS2812 灯带  — RMT 接口驱动可寻址 RGB LED (支持多颗串联)
 *
 * 【NVS 断电保持】
 *   灯光状态会持久化存储在 NVS 中。每次状态变更时自动保存，
 *   设备重启后自动恢复到断电前的灯光效果。
 *   NVS 命名空间："led_state"，存储两个 blob：pwm_state / ws2812_state。
 *   如果 NVS 读取失败（首次启动或数据损坏），使用默认值（全部关闭）。
 */

#include "led_control.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "led_strip.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "led_control";

// 引脚配置（从 Kconfig / menuconfig 读取）
#define PWM_R_GPIO   CONFIG_LED_PWM_R_GPIO
#define PWM_G_GPIO   CONFIG_LED_PWM_G_GPIO
#define PWM_B_GPIO   CONFIG_LED_PWM_B_GPIO
#define WS2812_GPIO  CONFIG_LED_WS2812_GPIO
#define WS2812_NUM   CONFIG_LED_WS2812_NUM

// 当前灯光状态（运行时 RAM 副本）
static led_state_t pwm_state    = {.state = false, .brightness = 255, .red = 255, .green = 255, .blue = 255};
static led_state_t ws2812_state = {.state = false, .brightness = 255, .red = 255, .green = 255, .blue = 255};

// WS2812 驱动句柄
static led_strip_handle_t ws2812_strip = NULL;

/** @brief NVS 命名空间名称，用于存储灯光状态 */
#define LED_NVS_NAMESPACE "led_state"
/** @brief PWM 灯状态的 NVS 键名 */
#define NVS_KEY_PWM      "pwm_state"
/** @brief WS2812 灯带状态的 NVS 键名 */
#define NVS_KEY_WS2812   "ws2812_state"

/*===========================================================================
 * 硬件初始化
 *===========================================================================*/

/**
 * @brief  初始化三通道 LEDC PWM 驱动
 *
 * 配置：
 *   - 定时器：LEDC_TIMER_0，8 位分辨率（0-255），频率 5 kHz（避免人眼可见闪烁）
 *   - 通道 0/1/2 分别对应 R/G/B 引脚，初始占空比均为 0（关闭状态）
 */
static void ledc_pwm_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,      // 0-255 分辨率，与 RGB 分量匹配
        .freq_hz         = 5000,                   // 5 kHz PWM，消除闪烁
        .clk_cfg         = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ch[3] = {
        {.gpio_num = PWM_R_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0},
        {.gpio_num = PWM_G_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1, .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0},
        {.gpio_num = PWM_B_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_2, .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0}
    };
    for (int i = 0; i < 3; i++) {
        ESP_ERROR_CHECK(ledc_channel_config(&ch[i]));
    }
    ESP_LOGI(TAG, "PWM LED initialized on pins %d,%d,%d", PWM_R_GPIO, PWM_G_GPIO, PWM_B_GPIO);
}

/**
 * @brief  初始化 WS2812 可寻址 LED 灯带
 *
 * 使用 RMT（Remote Control Transceiver）接口驱动 WS2812 时序。
 * 初始化后立即清除所有像素（熄灭状态）。
 * WS2812 通信协议对时序要求严格（ns 级），RMT 是 ESP32 上最可靠的驱动方式。
 */
static void ws2812_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num           = WS2812_GPIO,
        .max_leds                 = WS2812_NUM,
        .color_component_format   = LED_STRIP_COLOR_COMPONENT_FMT_GRB,  // WS2812 使用 GRB 色彩顺序
        .led_model                = LED_MODEL_WS2812,
        .flags.invert_out         = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  // 10 MHz RMT 分辨率
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &ws2812_strip));
    led_strip_clear(ws2812_strip);
    ESP_LOGI(TAG, "WS2812 initialized on GPIO %d, %d LEDs", WS2812_GPIO, WS2812_NUM);
}

/*===========================================================================
 * NVS 持久化 — 断电保持
 *===========================================================================*/

/**
 * @brief  将当前 PWM 灯和 WS2812 灯的状态保存到 NVS
 *
 * 每次灯光状态变更后自动调用，确保断电/重启后能恢复。
 * 保存失败（如 NVS 空间不足）只记录日志，不影响当前灯光工作。
 * 两个通道独立保存，一个保存失败不影响另一个。
 */
static void led_state_save_to_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(LED_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for saving: %s", esp_err_to_name(err));
        return;
    }

    // 保存 PWM 灯状态（以 blob 格式存储，结构体可直接作为二进制数据）
    err = nvs_set_blob(nvs_handle, NVS_KEY_PWM, &pwm_state, sizeof(led_state_t));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save PWM state: %s", esp_err_to_name(err));
    }

    // 保存 WS2812 灯带状态
    err = nvs_set_blob(nvs_handle, NVS_KEY_WS2812, &ws2812_state, sizeof(led_state_t));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save WS2812 state: %s", esp_err_to_name(err));
    }

    // nvs_commit 将缓存写入 Flash，确保断电不丢失
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

/**
 * @brief  从 NVS 恢复上次保存的灯光状态
 *
 * 在 led_control_init() 中调用，硬件初始化完毕后恢复。
 * 如果 NVS 中无数据（首次启动或未保存过），使用默认状态（关闭）。
 * 恢复成功后，将状态应用到硬件（LEDC PWM 占空比 / WS2812 像素颜色）。
 */
static void led_state_load_from_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(LED_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        // 首次启动或 NVS 未初始化，使用默认值（关闭状态）
        ESP_LOGI(TAG, "No saved LED state found, using defaults (OFF)");
        return;
    }

    // 恢复 PWM 灯状态
    led_state_t loaded_state;
    size_t size = sizeof(led_state_t);
    err = nvs_get_blob(nvs_handle, NVS_KEY_PWM, &loaded_state, &size);
    if (err == ESP_OK && size == sizeof(led_state_t)) {
        pwm_state = loaded_state;
        ESP_LOGI(TAG, "Restored PWM state: %s, brightness=%d, RGB=(%d,%d,%d)",
                 pwm_state.state ? "ON" : "OFF",
                 pwm_state.brightness,
                 pwm_state.red, pwm_state.green, pwm_state.blue);
        // 立即应用到硬件
        led_pwm_set_state(&pwm_state);
    } else {
        ESP_LOGI(TAG, "No saved PWM state or data corrupted, using defaults");
    }

    // 恢复 WS2812 灯带状态
    err = nvs_get_blob(nvs_handle, NVS_KEY_WS2812, &loaded_state, &size);
    if (err == ESP_OK && size == sizeof(led_state_t)) {
        ws2812_state = loaded_state;
        ESP_LOGI(TAG, "Restored WS2812 state: %s, brightness=%d, RGB=(%d,%d,%d)",
                 ws2812_state.state ? "ON" : "OFF",
                 ws2812_state.brightness,
                 ws2812_state.red, ws2812_state.green, ws2812_state.blue);
        // 立即应用到硬件
        led_ws2812_set_state(&ws2812_state);
    } else {
        ESP_LOGI(TAG, "No saved WS2812 state or data corrupted, using defaults");
    }

    nvs_close(nvs_handle);
}

/*===========================================================================
 * 公开 API
 *===========================================================================*/

/**
 * @brief  初始化灯光控制模块
 *
 * 启动流程：
 *   1. 初始化 LEDC PWM 硬件（三通道，占空比初始为 0）
 *   2. 初始化 WS2812 RMT 硬件（清除所有像素）
 *   3. 从 NVS 恢复上次保存的灯光状态并应用到硬件
 *
 * @note  必须在 nvs_flash_init() 之后调用（NVS 需要先初始化）
 */
void led_control_init(void)
{
    ledc_pwm_init();
    ws2812_init();

    // 从 NVS 恢复断电前的灯光状态，实现断电保持
    led_state_load_from_nvs();
}

/**
 * @brief  设置 PWM RGB 灯的亮度/颜色/开关状态
 *
 * 副作用：
 *   - 写入 LEDC 三通道 PWM 占空比到硬件
 *   - 自动保存新状态到 NVS（断电后可恢复）
 *
 * 亮度公式：
 *   if (!state) → duty = 0（所有通道）
 *   else        → duty = (color × brightness) / 255
 *   即：关闭时完全熄灭，开启时颜色分量按亮度比例缩放到 PWM 占空比
 *
 * @param state  目标状态指针，为 NULL 时安全返回（不执行任何操作）
 */
void led_pwm_set_state(const led_state_t *state)
{
    if (!state) return;
    pwm_state = *state;

    // 计算硬件占空比：关闭时全零，开启时 颜色 × 亮度 / 255
    uint8_t r = state->state ? (state->red   * state->brightness) / 255 : 0;
    uint8_t g = state->state ? (state->green * state->brightness) / 255 : 0;
    uint8_t b = state->state ? (state->blue  * state->brightness) / 255 : 0;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, r);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, g);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, b);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);

    // 持久化：任何状态变更后立即保存到 NVS，确保断电不丢失
    led_state_save_to_nvs();
}

/**
 * @brief  设置 WS2812 灯带的亮度/颜色/开关状态
 *
 * 副作用：
 *   - 刷新所有 WS2812 像素的颜色数据
 *   - 自动保存新状态到 NVS（断电后可恢复）
 *
 * 每个像素的计算公式与 PWM 一致：
 *   if (!state) → 清除所有像素（调用 led_strip_clear）
 *   else        → 每个像素设置为 (r×brightness/255, g×brightness/255, b×brightness/255)
 *
 * @param state  目标状态指针，为 NULL 时安全返回
 */
void led_ws2812_set_state(const led_state_t *state)
{
    if (!ws2812_strip || !state) return;
    ws2812_state = *state;

    if (!state->state) {
        // 关闭状态：清除所有像素
        led_strip_clear(ws2812_strip);
    } else {
        // 开启状态：按亮度缩放后设置每个像素
        uint8_t r = (state->red   * state->brightness) / 255;
        uint8_t g = (state->green * state->brightness) / 255;
        uint8_t b = (state->blue  * state->brightness) / 255;
        for (int i = 0; i < WS2812_NUM; i++) {
            led_strip_set_pixel(ws2812_strip, i, r, g, b);
        }
    }
    led_strip_refresh(ws2812_strip);

    // 持久化：任何状态变更后立即保存到 NVS
    led_state_save_to_nvs();
}

/**
 * @brief  获取 PWM 灯当前状态指针（只读共享访问）
 *
 * @return 指向内部 pwm_state 的指针，请勿 free
 */
led_state_t *led_pwm_get_state(void)
{
    return &pwm_state;
}

/**
 * @brief  获取 WS2812 灯带当前状态指针（只读共享访问）
 *
 * @return 指向内部 ws2812_state 的指针，请勿 free
 */
led_state_t *led_ws2812_get_state(void)
{
    return &ws2812_state;
}