# ESP32-C3 OTA 在线升级方案

## Context

当前项目是基于 ESP-IDF v6.0.1 的 ESP32-C3 智能灯光控制器，已集成 Wi-Fi、MQTT/Home Assistant。现需新增 OTA 固件升级能力：
- 可通过 Web 界面上传固件
- 设备自动定时检查并升级
- 支持手动降级到任意旧版本
- 需要阿里云 ECS 服务器端支持

**现有基础设施可直接复用：**
- MQTT Broker 已在阿里云运行（`47.121.26.212:1883`），可用于 OTA 命令下发和进度上报
- 分区表已有 OTA 双分区（`ota_0`/`ota_1` + `otadata`），不需要修改
- device_id 模块已预留 OTA 相关 MQTT Topic 变量（待填充）

## 整体架构

```
┌──────────────────────────────────────────────────────┐
│  阿里云 ECS                                          │
│  ┌──────────────────────────────────────────┐        │
│  │  Nginx (反向代理 + HTTPS)                 │        │
│  │  ├─ /api/*     → Gunicorn + Flask (API)  │        │
│  │  └─ /          → Flask (Web UI)          │        │
│  │       │                                   │        │
│  │       ├─ SQLite: 固件元数据/设备状态       │        │
│  │       ├─ 文件存储: firmware/*.bin          │        │
│  │       └─ MQTT Client: 推送更新通知         │        │
│  └──────────────────────────────────────────┘        │
│                      ↕ HTTPS / MQTT                   │
└──────────────────────────────────────────────────────┘
                       ↕
┌──────────────────────────────────────────────────────┐
│  ESP32-C3                                            │
│  ┌──────────────────────────────────────────┐        │
│  │  ota_handler (新组件)                     │        │
│  │  ├─ 定时任务: 每 2h 检查新版本            │        │
│  │  ├─ MQTT 订阅: 接收升级/降级命令          │        │
│  │  ├─ esp_https_ota: 下载固件+写入ota分区   │        │
│  │  ├─ NVS: 版本号/升级策略持久化             │        │
│  │  └─ MQTT 发布: 进度/结果上报              │        │
│  └──────────────────────────────────────────┘        │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐             │
│  │ ota_0    │ │ ota_1    │ │ factory  │             │
│  │ (app分区)│ │ (app分区)│ │ (备援)   │             │
│  └──────────┘ └──────────┘ └──────────┘             │
└──────────────────────────────────────────────────────┘
```

## 实施步骤

### 第一阶段：服务器端 — 固件管理平台

#### 1.1 技术栈

| 层级 | 技术 | 说明 |
|------|------|------|
| Web 框架 | Flask + Bootstrap 5 | 简单够用，快速开发 |
| API 服务 | Gunicorn + Flask | WSGI 生产部署 |
| 数据库 | SQLite | 存储固件元数据和设备记录，无需额外安装 |
| 反向代理 | Nginx | HTTPS 终端 + 静态文件 + API 代理 |
| MQTT | paho-mqtt (Python) | 推送 OTA 通知给设备 |
| 部署 | systemd + venv | 简单可靠，无需 Docker（用户服务器未安装） |

#### 1.2 目录结构 (`ota-server/`)

```
ota-server/
├── app.py               # Flask 应用主入口
├── models.py            # SQLite 数据模型
├── mqtt_client.py       # 服务器端 MQTT 客户端（推送升级通知）
├── templates/
│   ├── base.html        # Bootstrap 基础模板
│   ├── index.html       # 固件列表 + 上传
│   ├── devices.html     # 设备管理页面
│   └── login.html       # 简单登录
├── static/
│   └── (Bootstrap CSS/JS)
├── uploads/             # 固件文件存储目录
├── ota.db               # SQLite 数据库（自动创建）
├── requirements.txt     # Python 依赖
├── config.py            # 配置（Broker地址、账号密码）
├── ota-server.service   # systemd 服务文件
└── deploy.sh            # 一键部署脚本
```

#### 1.3 API 设计

| 端点 | 方法 | 说明 | 鉴权 |
|------|------|------|------|
| `/api/firmware/check` | GET | 检查更新 `?device_id=xxx&version=x.y.z` | Token |
| `/api/firmware/download/<id>` | GET | 下载固件 .bin 文件 | Token |
| `/api/firmware/upload` | POST | Web UI 上传新固件 | Session |
| `/api/firmware/list` | GET | Web UI 固件列表 | Session |
| `/api/firmware/delete/<id>` | POST | Web UI 删除版本 | Session |
| `/api/devices` | GET | Web UI 设备列表及状态 | Session |
| `/api/auth/login` | POST | Web UI 登录 | - |
| `/api/firmware/<id>/push` | POST | 通过 MQTT 推送升级命令给指定设备 | Session |

#### 1.4 数据模型 (SQLite)

```sql
-- 固件版本表
CREATE TABLE firmware (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    version TEXT NOT NULL,           -- 语义版本号，如 "3.0.1"
    filename TEXT NOT NULL,          -- 存储文件名
    file_size INTEGER,              -- 文件大小 (bytes)
    sha256 TEXT,                    -- 校验和
    changelog TEXT,                 -- 更新日志
    is_latest BOOLEAN DEFAULT 0,    -- 是否最新稳定版
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- 设备表
CREATE TABLE devices (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT UNIQUE NOT NULL,  -- MAC 地址
    device_name TEXT,                -- 设备名称
    current_version TEXT,            -- 当前固件版本
    target_version TEXT,             -- 目标版本（升级中）
    status TEXT DEFAULT 'online',    -- online/offline/upgrading
    last_seen TIMESTAMP,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- 升级记录表
CREATE TABLE upgrade_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT,
    from_version TEXT,
    to_version TEXT,
    status TEXT,                     -- success/failed/rollback
    started_at TIMESTAMP,
    completed_at TIMESTAMP
);
```

#### 1.5 部署方式

在阿里云 ECS 上以 systemd 服务运行：

```bash
# 一键部署
git clone <repo> /opt/ota-server
cd /opt/ota-server
bash deploy.sh   # 自动: python venv → pip install → systemd → nginx config
```

Nginx 配置示例（`/etc/nginx/sites-enabled/ota-server`）：
```nginx
server {
    listen 80;
    server_name your-domain.com;
    client_max_body_size 10M;    # 固件文件限制 10MB

    location / {
        proxy_pass http://127.0.0.1:5000;
        proxy_set_header Host $host;
    }
}
```

### 第二阶段：ESP32 端 — ota_handler 组件

#### 2.1 新增文件

```
components/ota_handler/
├── CMakeLists.txt
├── idf_component.yml
├── Kconfig                    # menuconfig 配置项
├── include/
│   └── ota_handler.h          # 公开 API
└── src/
    └── ota_handler.c          # 核心实现
```

#### 2.2 核心功能

**A. 定时自动检查更新**

```
每 CONFIG_OTA_CHECK_INTERVAL_MIN 分钟:
  1. HTTPS GET /api/firmware/check?device_id=xxx&version=x.y.z
  2. 服务器返回 { "has_update": true, "version": "3.0.1", "url": "...", "size": xxx }
  3. 如果有新版本 → 触发下载升级
```

**B. MQTT 命令处理**

订阅 `ota_command_topic`，支持 JSON 命令：
```json
{"action": "upgrade", "version": "3.0.1"}    // 升级到指定版本
{"action": "downgrade", "version": "2.0.0"}  // 降级到旧版本
{"action": "check"}                           // 立即检查更新
```

**C. 固件下载与刷写**

使用 ESP-IDF 的 `esp_https_ota` API：
- 支持 HTTPS 下载（使用 ESP x509 Certificate Bundle）
- 自动写入对侧 OTA 分区
- 校验 SHA256
- 下载过程中通过 MQTT 上报进度百分比

**D. 回滚支持**

利用 ESP-IDF 内置回滚机制：
- `esp_ota_mark_app_valid_cancel_rollback()` — 新固件启动正常后调用
- 如果新固件启动后崩溃（未调用上述函数），bootloader 自动回滚到上一版本
- 手动降级使用 `esp_ota_set_boot_partition()` 强制切换启动分区

**E. 版本号管理**

- 版本号编译期注入：`-DFIRMWARE_VERSION=\"3.0.0\"`（通过 CMakeLists.txt 的 Git tag 或手动定义）
- 当前版本存 NVS：`"ota"` namespace → `"fw_version"` key
- 启动时比对编译版本 vs NVS 版本，判断是否刚完成 OTA

#### 2.3 ota_handler.h API

```c
// 初始化 OTA 模块（启动定时检查任务 + 订阅 MQTT 命令）
void ota_handler_init(void);

// 获取当前固件版本号
const char* ota_get_version(void);

// 触发立即检查更新（供 MQTT 命令调用）
void ota_check_now(void);

// OTA 状态回调注册（供外部模块监听进度）
typedef void (*ota_progress_cb_t)(int progress_pct, const char *status);
void ota_register_progress_callback(ota_progress_cb_t cb);
```

#### 2.4 集成到 app_main.c

```c
void app_main(void)
{
    // ... 现有初始化 ...
    device_id_init();       // Step 2
    led_control_init();     // Step 3
    wifi_manager_init();    // Step 4 (阻塞)
    mqtt_ha_init();         // Step 5
    ota_handler_init();     // Step 5.5: OTA 初始化（WiFi+MQTT 就绪后）
    rssi_reporter_start();  // Step 6
}
```

### 第三阶段：设备 ID 模块补全

在 `device_id.c` 的 `device_id_init()` 中填充 OTA topic（已在 .h 中声明）：

```c
// 第 6 步之后追加 — OTA 主题
snprintf(ota_command_topic, sizeof(ota_command_topic),
         "homeassistant/ota/%s/command", base_device_id);
snprintf(ota_button_config_topic, sizeof(ota_button_config_topic),
         "homeassistant/ota/%s/config", base_device_id);
snprintf(ota_progress_topic, sizeof(ota_progress_topic),
         "homeassistant/ota/%s/progress", base_device_id);
```

## 涉及文件汇总

| 文件 | 操作 | 说明 |
|------|------|------|
| `ota-server/` (新建目录) | 新建 | 完整服务端项目 |
| `components/ota_handler/` (新建) | 新建 | ESP32 OTA 组件 |
| `components/ota_handler/CMakeLists.txt` | 新建 | 依赖 esp_https_ota, mqtt_ha, nvs_flash, cjson |
| `components/ota_handler/idf_component.yml` | 新建 | 依赖声明 |
| `components/ota_handler/Kconfig` | 新建 | 可配置参数（检查间隔、服务器URL等） |
| `components/device_id/src/device_id.c` | 修改 | 填充 3 个 OTA topic 变量 |
| `main/app_main.c` | 修改 | 在 mqtt_ha_init() 后添加 ota_handler_init() |
| `main/CMakeLists.txt` | 修改 | REQUIRES 增加 ota_handler |
| `CMakeLists.txt` (根) | 修改 | 添加 FIRMWARE_VERSION 编译宏 |

## 关键配置参数 (Kconfig)

```kconfig
menu "OTA Upgrade Configuration"
    config OTA_SERVER_URL
        string "OTA Server Base URL"
        default "http://your-server.com"
    config OTA_CHECK_INTERVAL_MIN
        int "Auto check interval (minutes)"
        default 120
    config OTA_AUTO_UPGRADE
        bool "Enable auto upgrade"
        default y
    config OTA_API_TOKEN
        string "API Token for device authentication"
        default ""
endmenu
```

## 验证方式

1. **服务端验证**：
   - 部署后访问 Web 页面 → 上传测试固件 → 验证列表显示正确
   - `curl /api/firmware/check` 验证 API 返回正确 JSON

2. **ESP32 端验证**：
   - 查看串口日志：`OTA check: current=v3.0.0, latest=v3.0.1`
   - 自动下载升级 → 重启 → 运行新版本
   - MQTT 查看进度 topic：`homeassistant/ota/<id>/progress` 发布进度百分比
   - 手动降级测试：通过 MQTT 发送降级命令 → 设备回退到旧版本

3. **回滚验证**：
   - 烧录一个带崩溃 bug 的"坏"固件 → OTA 升级 → 设备自动回滚 → 查看日志确认回滚成功
