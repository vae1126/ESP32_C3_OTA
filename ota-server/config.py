"""
OTA 固件管理平台 — 配置文件
部署时修改此文件中的参数
"""
import os

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

# ---- 服务器 ----
HOST = "0.0.0.0"
PORT = 5000
SECRET_KEY = "change-me-to-a-random-string"  # Flask session 加密密钥，请修改

# ---- 数据库 ----
DATABASE = os.path.join(BASE_DIR, "ota.db")

# ---- 固件存储 ----
UPLOAD_DIR = os.path.join(BASE_DIR, "uploads")
MAX_FIRMWARE_SIZE = 10 * 1024 * 1024  # 10MB

# ---- MQTT Broker ----
MQTT_BROKER = "47.121.26.212"
MQTT_PORT = 1883
MQTT_USERNAME = ""      # 如无密码留空
MQTT_PASSWORD = ""      # 如无密码留空
MQTT_TOPIC_PREFIX = "homeassistant/ota"  # 与 ESP32 端 topic 前缀一致

# ---- 设备认证 ----
API_TOKEN = "ota-api-token-2024"  # 设备请求 API 时携带的 token，请修改

# ---- 管理员账号 ----
ADMIN_USERNAME = "admin"
ADMIN_PASSWORD = "admin123"  # 生产环境请修改
