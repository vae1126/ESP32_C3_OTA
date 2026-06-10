/**
 * @file    mqtt_ha.c
 * @brief   MQTT + Home Assistant 集成模块实现
 *
 * 本文件实现了 ESP32 通过 MQTT 协议接入 Home Assistant 的完整逻辑：
 *
 *   【发现阶段】
 *   当 MQTT 连接成功后，自动向 Home Assistant 发送"发现配置"消息。
 *   HA 的 MQTT Discovery 机制会自动在界面上创建对应的实体，
 *   无需用户在 configuration.yaml 中手动配置。
 *
 *   【实体列表】
 *   - light/<device_id>_pwm       → PWM 驱动的 RGB 灯（如共阳/共阴灯珠）
 *   - light/<device_id>_ws2812    → WS2812 可寻址 RGB 灯带
 *   - sensor/<device_id>/rssi     → Wi-Fi RSSI 信号强度传感器
 *
 *   【命令处理】
 *   订阅每个实体对应的 command topic，接收 HA 发来的 JSON 格式命令：
 *     {
 *       "state": "ON"|"OFF",
 *       "brightness": 0-255,
 *       "color": {"r": 0-255, "g": 0-255, "b": 0-255}
 *     }
 *   解析后将状态写入 led_control 模块驱动硬件，并通过 state topic 反馈结果。
 *
 *   【状态反馈】
 *   每次灯光状态变更后，立即通过对应的 state topic 发布完整 JSON 状态。
 *   使用 retain 标志确保 HA 重新订阅时能立即获得最新状态。
 *
 *   【遗嘱机制（LWT）】
 *   配置 MQTT Last Will：当设备异常断开时，Broker 自动发布 "offline"
 *   到 availability 主题，Home Assistant 会将设备标记为"不可用"。
 *   正常连接后发布 "online" 恢复可用状态。
 */

#include "mqtt_ha.h"
#include "device_id.h"
#include "led_control.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "mqtt_ha";

/** @brief MQTT 客户端句柄，由 mqtt_ha_init() 创建并持有 */
static esp_mqtt_client_handle_t mqtt_client = NULL;

/*===========================================================================
 * 前向声明（静态函数）
 *===========================================================================*/

/** @brief 发布 PWM 灯的当前状态到 MQTT state topic */
static void publish_pwm_state(void);

/** @brief 发布 WS2812 灯带的当前状态到 MQTT state topic */
static void publish_ws2812_state(void);

/** @brief 解析并执行 PWM 灯的控制命令（来自 MQTT command topic） */
static void handle_pwm_command(const char *data, int data_len);

/** @brief 解析并执行 WS2812 灯带的控制命令（来自 MQTT command topic） */
static void handle_ws2812_command(const char *data, int data_len);

/** @brief MQTT 事件分发回调，处理连接、数据接收等事件 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data);

/*===========================================================================
 * 公开 API
 *===========================================================================*/

/**
 * @brief  获取 MQTT 客户端句柄（供其他模块使用）
 *
 * 典型用途：rssi_reporter 通过该句柄发布 RSSI 测量数据。
 *
 * @return esp_mqtt_client_handle_t  有效的客户端句柄或 NULL
 */
esp_mqtt_client_handle_t mqtt_ha_get_client(void)
{
    return mqtt_client;
}

/**
 * @brief  向 MQTT Broker 发布一条消息（线程安全）
 *
 * 说明：
 *  - 如果 mqtt_client 未初始化（为 NULL），函数安全返回，不执行任何操作。
 *    这避免了在 Wi-Fi 未连接或 MQTT 未就绪时发生空指针访问。
 *  - QoS 固定为 1（至少一次送达），retain 由调用方决定。
 *  - 通常用于：
 *      * 发布"发现配置"消息（retain = 1）
 *      * 发布设备在线/离线状态（retain = 1）
 *      * 发布灯光状态（retain = 1）
 *      * 发布 RSSI 读数（retain = 1）
 *
 * @param topic   MQTT 主题字符串
 * @param payload 消息体（JSON 字符串或纯文本）
 * @param retain  是否保留消息：1 = Broker 存储最后一条消息，新订阅者立即获取
 */
void mqtt_ha_publish(const char *topic, const char *payload, int retain)
{
    if (mqtt_client)
    {
        // QoS=1 确保至少一次送达，retain 由调用者决定
        esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, retain);
    }
}

/*===========================================================================
 * 状态发布函数
 *===========================================================================*/

/**
 * @brief  将 PWM RGB 灯的当前状态序列化为 JSON 并发布
 *
 * 发布的 JSON 格式示例：
 *   {
 *     "state": "ON",
 *     "brightness": 200,
 *     "color_mode": "rgb",
 *     "color": {"r": 255, "g": 128, "b": 0}
 *   }
 *
 * 使用 retain = 1 确保 Home Assistant 重新订阅时能立即获取最新状态。
 * 消息发布到全局变量 pwm_state_topic（由 device_id 模块初始化）。
 */
static void publish_pwm_state(void)
{
    // 从 led_control 模块获取 PWM 灯的当前硬件状态
    led_state_t *state = led_pwm_get_state();

    // 构建 JSON 对象
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", state->state ? "ON" : "OFF");
    cJSON_AddNumberToObject(root, "brightness", state->brightness);
    cJSON_AddStringToObject(root, "color_mode", "rgb");

    // 嵌套 color 对象
    cJSON *color = cJSON_CreateObject();
    cJSON_AddNumberToObject(color, "r", state->red);
    cJSON_AddNumberToObject(color, "g", state->green);
    cJSON_AddNumberToObject(color, "b", state->blue);
    cJSON_AddItemToObject(root, "color", color);

    // 序列化为字符串并发布，使用 retain 确保状态持久化
    char *state_str = cJSON_Print(root);
    if (state_str)
    {
        mqtt_ha_publish(pwm_state_topic, state_str, 1);
        free(state_str);   // cJSON_Print 内部 malloc，需手动释放
    }
    cJSON_Delete(root);
}

/**
 * @brief  将 WS2812 灯带的当前状态序列化为 JSON 并发布
 *
 * JSON 格式与 PWM 灯完全一致，仅发布到不同的 topic（ws2812_state_topic）。
 * 为保持与 Home Assistant 的兼容性，color_mode 固定为 "rgb"。
 */
static void publish_ws2812_state(void)
{
    led_state_t *state = led_ws2812_get_state();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", state->state ? "ON" : "OFF");
    cJSON_AddNumberToObject(root, "brightness", state->brightness);
    cJSON_AddStringToObject(root, "color_mode", "rgb");

    cJSON *color = cJSON_CreateObject();
    cJSON_AddNumberToObject(color, "r", state->red);
    cJSON_AddNumberToObject(color, "g", state->green);
    cJSON_AddNumberToObject(color, "b", state->blue);
    cJSON_AddItemToObject(root, "color", color);

    char *state_str = cJSON_Print(root);
    if (state_str)
    {
        mqtt_ha_publish(ws2812_state_topic, state_str, 1);
        free(state_str);
    }
    cJSON_Delete(root);
}

/*===========================================================================
 * 命令处理函数
 *===========================================================================*/

/**
 * @brief  解析并执行 PWM RGB 灯的 JSON 控制命令
 *
 * 从 MQTT command topic 接收到的消息通过此函数处理。
 * 支持的 JSON 字段（均为可选，按需更新）：
 *   - "state"       : "ON" 或 "OFF"，控制开关
 *   - "brightness"  : 0-255 整数，控制整体亮度
 *   - "color"       : {"r":0-255, "g":0-255, "b":0-255} 对象
 *
 * 设计要点：
 *   1. 增量更新：只有实际变更的字段才触发硬件更新，避免不必要的 LED 闪烁
 *   2. 范围裁剪：brightness 被限制在 [0, 255] 范围内，防止异常值
 *   3. 状态同步：更新硬件后立即调用 publish_pwm_state() 反馈结果
 *   4. 容错处理：JSON 解析失败时静默返回，不产生副作用
 *
 * 亮度计算公式（led_control 模块内部实现）：
 *   duty = state ? (color * brightness) / 255 : 0
 *   即：灯关闭时 duty 为 0；打开时颜色分量按亮度比例缩放
 *
 * @param data     指向接收到的原始 JSON 字符串（可能不含 '\0' 结尾）
 * @param data_len 数据长度（字节）
 */
static void handle_pwm_command(const char *data, int data_len)
{
    // 解析 JSON，使用带长度的版本避免字符串截断问题
    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (!root)
        return;  // JSON 无效，静默丢弃

    // 获取当前状态作为基准，new_state 用作增量修改
    led_state_t *state = led_pwm_get_state();
    led_state_t new_state = *state;
    bool need_update = false;

    // ---- 解析 "state" 字段（开关） ----
    cJSON *state_item = cJSON_GetObjectItem(root, "state");
    if (cJSON_IsString(state_item))
    {
        bool s = (strcmp(state_item->valuestring, "ON") == 0);
        if (s != state->state)
        {
            new_state.state = s;
            need_update = true;
        }
    }

    // ---- 解析 "brightness" 字段（0-255） ----
    cJSON *brightness_item = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(brightness_item))
    {
        int b = brightness_item->valueint;
        // 裁剪到有效范围，防御异常输入
        if (b < 0)  b = 0;
        if (b > 255) b = 255;
        if (b != state->brightness)
        {
            new_state.brightness = b;
            need_update = true;
        }
    }

    // ---- 解析 "color" 字段（RGB 对象） ----
    cJSON *color_item = cJSON_GetObjectItem(root, "color");
    if (cJSON_IsObject(color_item))
    {
        cJSON *r = cJSON_GetObjectItem(color_item, "r");
        cJSON *g = cJSON_GetObjectItem(color_item, "g");
        cJSON *b = cJSON_GetObjectItem(color_item, "b");
        if (cJSON_IsNumber(r) && cJSON_IsNumber(g) && cJSON_IsNumber(b))
        {
            uint8_t red   = r->valueint;
            uint8_t green = g->valueint;
            uint8_t blue  = b->valueint;
            if (red != state->red || green != state->green || blue != state->blue)
            {
                new_state.red   = red;
                new_state.green = green;
                new_state.blue  = blue;
                need_update = true;
            }
        }
    }

    // 仅在确实有变化时更新硬件并反馈状态，避免不必要的 I/O 操作
    if (need_update)
    {
        *state = new_state;
        led_pwm_set_state(state);      // 写入 LEDC 硬件 PWM 占空比
        publish_pwm_state();          // 反馈新状态到 Home Assistant
    }

    cJSON_Delete(root);
}

/**
 * @brief  解析并执行 WS2812 灯带的 JSON 控制命令
 *
 * 与 handle_pwm_command() 逻辑完全一致，区别在于：
 *   - 操作的硬件对象是 WS2812 灯带（通过 RMT 接口驱动）
 *   - 状态读写使用 led_ws2812_* 系列函数
 *   - 反馈状态发布到 ws2812_state_topic
 *
 * 注意：Home Assistant 将 PWM 灯和 WS2812 灯带视为两个独立的 light 实体，
 *       因此需要分别处理各自的 command topic。
 *
 * @param data     指向接收到的原始 JSON 字符串
 * @param data_len 数据长度（字节）
 */
static void handle_ws2812_command(const char *data, int data_len)
{
    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (!root)
        return;

    led_state_t *state = led_ws2812_get_state();
    led_state_t new_state = *state;
    bool need_update = false;

    // 解析开关状态
    cJSON *state_item = cJSON_GetObjectItem(root, "state");
    if (cJSON_IsString(state_item))
    {
        bool s = (strcmp(state_item->valuestring, "ON") == 0);
        if (s != state->state)
        {
            new_state.state = s;
            need_update = true;
        }
    }

    // 解析亮度（0-255，裁剪到有效范围）
    cJSON *brightness_item = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(brightness_item))
    {
        int b = brightness_item->valueint;
        if (b < 0)  b = 0;
        if (b > 255) b = 255;
        if (b != state->brightness)
        {
            new_state.brightness = b;
            need_update = true;
        }
    }

    // 解析 RGB 颜色
    cJSON *color_item = cJSON_GetObjectItem(root, "color");
    if (cJSON_IsObject(color_item))
    {
        cJSON *r = cJSON_GetObjectItem(color_item, "r");
        cJSON *g = cJSON_GetObjectItem(color_item, "g");
        cJSON *b = cJSON_GetObjectItem(color_item, "b");
        if (cJSON_IsNumber(r) && cJSON_IsNumber(g) && cJSON_IsNumber(b))
        {
            uint8_t red   = r->valueint;
            uint8_t green = g->valueint;
            uint8_t blue  = b->valueint;
            if (red != state->red || green != state->green || blue != state->blue)
            {
                new_state.red   = red;
                new_state.green = green;
                new_state.blue  = blue;
                need_update = true;
            }
        }
    }

    // 仅在状态有实际变化时驱动硬件
    if (need_update)
    {
        *state = new_state;
        led_ws2812_set_state(state);  // 刷新 WS2812 灯带所有像素
        publish_ws2812_state();       // 反馈新状态
    }

    cJSON_Delete(root);
}

/*===========================================================================
 * MQTT 事件处理
 *===========================================================================*/

/**
 * @brief  MQTT 客户端事件回调函数（esp_mqtt_client 事件循环的核心入口）
 *
 * 处理两类主要事件：
 *
 *   【MQTT_EVENT_CONNECTED — 连接成功】
 *   Broker 连接建立后触发，此时执行一次性初始化工作：
 *     1. 发布在线状态（"online" → availability topic）
 *     2. 订阅 PWM 灯和 WS2812 灯的命令主题（command topic）
 *     3. 为每个实体发送 Home Assistant MQTT Discovery 配置消息
 *        （包含名称、唯一 ID、命令/状态主题、设备信息等）
 *     4. 发布当前灯光状态，同步硬件实际状态到 Home Assistant
 *
 *   注意：发现配置使用 retain = 1，确保 HA 重启后能自动恢复实体注册。
 *
 *   【MQTT_EVENT_DATA — 收到消息】
 *   根据消息的 topic 判断是哪个实体的命令：
 *     - 匹配 pwm_command_topic     → handle_pwm_command()
 *     - 匹配 ws2812_command_topic  → handle_ws2812_command()
 *
 * @param handler_args 注册回调时传入的用户参数（此处未使用，为 NULL）
 * @param base         事件基（ESP-IDF 事件系统概念，此处固定为 MQTT_EVENT）
 * @param event_id     具体事件 ID，参见 esp_mqtt_event_id_t 枚举
 * @param event_data   指向 esp_mqtt_event_t 结构体的事件数据
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    /*----------------------------------------------------------------------
     * MQTT 连接成功 — 初始化 Home Assistant 集成
     *--------------------------------------------------------------------*/
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");

        // 1. 发布在线状态，Home Assistant 据此将设备标记为"可用"
        mqtt_ha_publish(device_avail_topic, "online", 1);

        // 2. 订阅两个灯的命令主题，QoS=1
        esp_mqtt_client_subscribe(client, pwm_command_topic, 1);
        esp_mqtt_client_subscribe(client, ws2812_command_topic, 1);

        /*------------------------------------------------------------------
         * 3. 发布 PWM RGB 灯的 Home Assistant 发现配置
         *
         * Home Assistant MQTT Discovery 规范说明：
         *   消息发布到 <discovery_prefix>/<component>/<object_id>/config
         *   本项目中 discovery_prefix = "homeassistant"（HA 默认值）
         *
         * 关键字段说明：
         *   name         — 实体在 HA 界面中显示的名称
         *   unique_id    — 全局唯一标识，必须唯一，否则实体会被覆盖
         *   cmd_t        — 接收命令的 topic（HA 向此 topic 发布控制指令）
         *   stat_t       — 状态上报的 topic（设备向此 topic 发布状态）
         *   schema       — 命令格式，"json" 表示使用 JSON 命令
         *   brightness   — true 表示支持亮度调节
         *   supported_color_modes — 支持的颜色模式列表
         *   optimistic   — false 表示设备会反馈状态（非乐观模式）
         *   avty_t       — availability topic，用于在线/离线检测
         *   device       — 设备信息对象，将两个 light 实体关联到同一物理设备
         *------------------------------------------------------------------*/
        {
            cJSON *cfg = cJSON_CreateObject();
            cJSON_AddStringToObject(cfg, "name", "PWM RGB Light");
            cJSON_AddStringToObject(cfg, "unique_id", base_device_id);
            cJSON_AddStringToObject(cfg, "cmd_t", pwm_command_topic);
            cJSON_AddStringToObject(cfg, "stat_t", pwm_state_topic);
            cJSON_AddStringToObject(cfg, "schema", "json");
            cJSON_AddBoolToObject(cfg, "brightness", true);

            // 支持的色彩模式：RGB（通过红绿蓝三通道独立调节）
            cJSON *cm = cJSON_CreateArray();
            cJSON_AddItemToArray(cm, cJSON_CreateString("rgb"));
            cJSON_AddItemToObject(cfg, "supported_color_modes", cm);

            // optimistic=false 表示设备会主动反馈状态，HA 等待状态确认
            cJSON_AddBoolToObject(cfg, "optimistic", false);
            cJSON_AddStringToObject(cfg, "avty_t", device_avail_topic);

            // 设备信息：将 PWM 灯和 WS2812 灯关联到同一物理 ESP32 设备
            cJSON *dev = cJSON_CreateObject();
            cJSON_AddStringToObject(dev, "identifiers", base_device_id);
            cJSON_AddStringToObject(dev, "name", base_device_name);
            cJSON_AddStringToObject(dev, "model", "ESP32 Dual RGB Light");
            cJSON_AddStringToObject(dev, "manufacturer", "Espressif");
            cJSON_AddItemToObject(cfg, "device", dev);

            char *cfg_str = cJSON_Print(cfg);
            if (cfg_str)
            {
                mqtt_ha_publish(pwm_config_topic, cfg_str, 1);
                free(cfg_str);
            }
            cJSON_Delete(cfg);
        }

        /*------------------------------------------------------------------
         * 4. 发布 WS2812 灯带的 Home Assistant 发现配置
         *
         * 与 PWM 灯类似，区别在于：
         *   - unique_id 添加 "_ws2812" 后缀，确保与 PWM 灯实体不冲突
         *   - command/state topic 使用 ws2812 专属主题
         *   - 共享同一个 device 信息（通过 base_device_id 关联）
         *------------------------------------------------------------------*/
        {
            cJSON *cfg = cJSON_CreateObject();
            cJSON_AddStringToObject(cfg, "name", "WS2812 LED Strip");

            // unique_id 必须在 HA 中唯一，添加后缀避免与 PWM 灯冲突
            char uid[32];
            snprintf(uid, sizeof(uid), "%s_ws2812", base_device_id);
            cJSON_AddStringToObject(cfg, "unique_id", uid);

            cJSON_AddStringToObject(cfg, "cmd_t", ws2812_command_topic);
            cJSON_AddStringToObject(cfg, "stat_t", ws2812_state_topic);
            cJSON_AddStringToObject(cfg, "schema", "json");
            cJSON_AddBoolToObject(cfg, "brightness", true);

            cJSON *cm = cJSON_CreateArray();
            cJSON_AddItemToArray(cm, cJSON_CreateString("rgb"));
            cJSON_AddItemToObject(cfg, "supported_color_modes", cm);

            cJSON_AddBoolToObject(cfg, "optimistic", false);
            cJSON_AddStringToObject(cfg, "avty_t", device_avail_topic);

            cJSON *dev = cJSON_CreateObject();
            cJSON_AddStringToObject(dev, "identifiers", base_device_id);
            cJSON_AddItemToObject(cfg, "device", dev);

            char *cfg_str = cJSON_Print(cfg);
            if (cfg_str)
            {
                mqtt_ha_publish(ws2812_config_topic, cfg_str, 1);
                free(cfg_str);
            }
            cJSON_Delete(cfg);
        }

        /*------------------------------------------------------------------
         * 5. 发布 Wi-Fi RSSI 传感器的 Home Assistant 发现配置
         *
         * 这是一个诊断类实体（entity_category: "diagnostic"），
         * 不会出现在默认仪表盘上，用户需手动添加到 Lovelace 卡片。
         *
         * 关键字段：
         *   device_class      — "signal_strength" 信号强度类别
         *   state_class       — "measurement" 表示连续测量值
         *   entity_category   — "diagnostic" 诊断信息，默认隐藏
         *   unit_of_measurement — "dBm" 分贝毫瓦
         *------------------------------------------------------------------*/
        {
            cJSON *cfg = cJSON_CreateObject();
            cJSON_AddStringToObject(cfg, "name", "Wi-Fi RSSI");
            cJSON_AddStringToObject(cfg, "state_topic", rssi_state_topic);
            cJSON_AddStringToObject(cfg, "unit_of_measurement", "dBm");
            cJSON_AddStringToObject(cfg, "device_class", "signal_strength");
            cJSON_AddStringToObject(cfg, "state_class", "measurement");
            cJSON_AddStringToObject(cfg, "entity_category", "diagnostic");

            char uid[32];
            snprintf(uid, sizeof(uid), "wifi_rssi_%s", base_device_id);
            cJSON_AddStringToObject(cfg, "unique_id", uid);

            cJSON *dev = cJSON_CreateObject();
            cJSON_AddStringToObject(dev, "identifiers", base_device_id);
            cJSON_AddItemToObject(cfg, "device", dev);

            // 使用 PrintUnformatted 减少消息体积（诊断配置不需要可读性）
            char *cfg_str = cJSON_PrintUnformatted(cfg);
            if (cfg_str)
            {
                mqtt_ha_publish(rssi_config_topic, cfg_str, 1);
                free(cfg_str);
            }
            cJSON_Delete(cfg);
        }

        // 6. 发布当前灯光状态，确保 Home Assistant 显示与硬件实际一致
        publish_pwm_state();
        publish_ws2812_state();

        break;

    /*----------------------------------------------------------------------
     * MQTT 数据接收 — 命令分发
     *
     * 根据消息的 topic 长度和内容精确匹配，将命令路由到对应的处理器。
     * 使用 topic_len + strncmp 而非 strcmp，因为 MQTT 消息中 topic
     * 可能不以 '\0' 结尾。
     *--------------------------------------------------------------------*/
    case MQTT_EVENT_DATA:
        // 判断是否为 PWM 灯的命令
        if (event->topic_len == strlen(pwm_command_topic) &&
            strncmp(event->topic, pwm_command_topic, event->topic_len) == 0)
        {
            handle_pwm_command(event->data, event->data_len);
        }
        // 判断是否为 WS2812 灯的命令
        else if (event->topic_len == strlen(ws2812_command_topic) &&
                 strncmp(event->topic, ws2812_command_topic, event->topic_len) == 0)
        {
            handle_ws2812_command(event->data, event->data_len);
        }
        // （可在此处扩展更多 command topic 的处理）

        break;

    /*----------------------------------------------------------------------
     * 其他事件（DISCONNECTED、ERROR、BEFORE_CONNECT 等）
     * 由 esp_mqtt_client 内部自动处理重连，此处不需要额外逻辑
     *--------------------------------------------------------------------*/
    default:
        break;
    }
}

/*===========================================================================
 * 模块初始化
 *===========================================================================*/

/**
 * @brief  初始化 MQTT 客户端并启动 Home Assistant 集成
 *
 * 初始化流程：
 *   1. 读取 Wi-Fi STA MAC 地址，生成唯一的 MQTT Client ID
 *      （格式：ESP32_AABBCCDDEEFF）
 *   2. 构建 MQTT 配置结构体：
 *        - Broker URI 来自 Kconfig（menuconfig 中设置）
 *        - Client ID 基于 MAC 地址，确保同一 Broker 下不冲突
 *        - 协议版本 MQTT 3.1.1
 *        - 自动重连已启用
 *        - LWT（遗嘱消息）：设备异常断开时，Broker 自动发布 "offline"
 *   3. 创建并注册 MQTT 事件回调
 *   4. 启动 MQTT 客户端（异步，连接成功后会回调 MQTT_EVENT_CONNECTED）
 *
 * @note  LWT 使用 availability topic 和 retain 标志，
 *        确保 Home Assistant 能正确追踪设备的在线/离线状态
 */
void mqtt_ha_init(void)
{
    /*----------------------------------------------------------------------
     * 生成唯一的 MQTT Client ID
     *
     * 使用 Wi-Fi STA 接口的 MAC 地址（烧录在 eFuse 中，全球唯一）。
     * 格式：ESP32_XXXXXXXXXXXX（12 位大写十六进制）
     * 这确保了同一网络中多个 ESP32 设备不会产生 Client ID 冲突。
     *--------------------------------------------------------------------*/
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char client_id[32];
    snprintf(client_id, sizeof(client_id), "ESP32_%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /*----------------------------------------------------------------------
     * 配置 MQTT 客户端
     *
     * 使用 C99 指定初始化器（designated initializer）语法。
     * 关键配置项：
     *   - broker.address.uri          : MQTT Broker 地址（如 mqtt://192.168.1.100:1883）
     *   - credentials.client_id       : 客户端标识
     *   - session.protocol_ver        : MQTT 3.1.1（兼容性最广）
     *   - network.disable_auto_reconnect : false 即启用自动重连
     *   - session.last_will           : 遗嘱消息配置
     *       * topic  = device_avail_topic（设备可用性主题）
     *       * msg    = "offline"
     *       * qos    = 1
     *       * retain = true（持久化，确保新订阅者也能读到）
     *--------------------------------------------------------------------*/
    esp_mqtt_client_config_t mqtt_cfg = {0};
    mqtt_cfg.broker.address.uri = CONFIG_MQTT_BROKER_URL;  // 从 Kconfig / menuconfig 读取
    mqtt_cfg.credentials.client_id = client_id;
    mqtt_cfg.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    mqtt_cfg.network.disable_auto_reconnect = false;       // 启用自动重连
    mqtt_cfg.session.last_will.topic = device_avail_topic;  // 遗嘱主题：设备可用性
    mqtt_cfg.session.last_will.msg = "offline";             // 遗嘱消息：离线
    mqtt_cfg.session.last_will.qos = 1;                     // QoS 1
    mqtt_cfg.session.last_will.retain = true;               // 保留遗嘱消息

    // 创建 MQTT 客户端实例，注册事件处理器
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    // 启动 MQTT 客户端（异步方式，立即返回）
    // 客户端会在后台线程中尝试连接 Broker，成功后回调 MQTT_EVENT_CONNECTED
    esp_mqtt_client_start(mqtt_client);
}