"""
OTA 固件管理平台 — Flask 主应用
"""
import os
import hashlib
import functools
from datetime import datetime
from flask import Flask, request, jsonify, render_template, redirect, url_for, session, send_from_directory

import config
import models
from mqtt_client import init_mqtt, push_upgrade

app = Flask(__name__)
app.secret_key = config.SECRET_KEY


# ============================================================
# 装饰器：登录验证（Web UI 需要，API 使用 Token 鉴权）
# ============================================================

def login_required(f):
    @functools.wraps(f)
    def wrapper(*args, **kwargs):
        if not session.get("logged_in"):
            return redirect(url_for("login_page"))
        return f(*args, **kwargs)
    return wrapper


def api_token_required(f):
    @functools.wraps(f)
    def wrapper(*args, **kwargs):
        token = request.headers.get("X-API-Token") or request.args.get("token", "")
        if token != config.API_TOKEN:
            return jsonify({"error": "unauthorized"}), 401
        return f(*args, **kwargs)
    return wrapper


# ============================================================
# Web UI 路由
# ============================================================

@app.route("/")
@login_required
def index():
    firmwares = models.firmware_list()
    return render_template("index.html", firmwares=firmwares)


@app.route("/devices")
@login_required
def devices_page():
    devices = models.device_list()
    logs = models.log_list(limit=50)
    return render_template("devices.html", devices=devices, logs=logs)


@app.route("/login", methods=["GET", "POST"])
def login_page():
    if request.method == "POST":
        username = request.form.get("username", "")
        password = request.form.get("password", "")
        if username == config.ADMIN_USERNAME and password == config.ADMIN_PASSWORD:
            session["logged_in"] = True
            return redirect(url_for("index"))
        return render_template("login.html", error="账号或密码错误")
    return render_template("login.html")


@app.route("/logout")
def logout():
    session.clear()
    return redirect(url_for("login_page"))


# ============================================================
# 设备 API（供 ESP32 调用，Token 鉴权）
# ============================================================

@app.route("/api/firmware/check")
@api_token_required
def api_check_update():
    """ESP32 检查是否有新版本"""
    device_id = request.args.get("device_id", "")
    current_version = request.args.get("version", "0.0.0")

    # 更新设备在线状态
    models.device_update(device_id, current_version=current_version, status="online")

    latest = models.firmware_get_latest()
    if not latest:
        return jsonify({"has_update": False, "message": "no firmware available"})

    has_update = _version_greater(latest["version"], current_version)
    return jsonify({
        "has_update": has_update,
        "current_version": current_version,
        "latest_version": latest["version"],
        "file_size": latest["file_size"],
        "changelog": latest["changelog"],
        "sha256": latest["sha256"],
        "url": f"/api/firmware/download/{latest['id']}"
    })


@app.route("/api/firmware/download/<int:fw_id>")
@api_token_required
def api_download(fw_id):
    """ESP32 下载固件文件"""
    fw = models.firmware_get_by_id(fw_id)
    if not fw:
        return jsonify({"error": "not found"}), 404

    # 记录升级日志
    device_id = request.args.get("device_id", "unknown")
    models.log_add(device_id, request.args.get("from_version", ""), fw["version"], "downloading")

    return send_from_directory(
        config.UPLOAD_DIR, fw["filename"],
        as_attachment=True,
        download_name=fw["filename"]
    )


@app.route("/api/device/status", methods=["POST"])
@api_token_required
def api_device_status():
    """ESP32 上报升级结果"""
    data = request.get_json(force=True, silent=True) or {}
    device_id = data.get("device_id", "")
    status = data.get("status", "online")
    version = data.get("version", "")

    models.device_update(device_id, current_version=version, status=status)
    if status in ("success", "failed", "rollback"):
        models.log_update(device_id, status)

    return jsonify({"ok": True})


# ============================================================
# 管理 API（Web UI 调用，Session 鉴权）
# ============================================================

@app.route("/api/firmware/upload", methods=["POST"])
@login_required
def api_upload():
    """上传新固件"""
    file = request.files.get("firmware")
    if not file or not file.filename.endswith(".bin"):
        return jsonify({"error": "请选择 .bin 固件文件"}), 400

    version = request.form.get("version", "").strip()
    if not version:
        return jsonify({"error": "请输入版本号"}), 400

    changelog = request.form.get("changelog", "").strip()
    is_latest = 1 if request.form.get("is_latest") == "1" else 0

    # 保存文件
    safe_name = f"firmware_v{version}_{_ts()}.bin"
    filepath = os.path.join(config.UPLOAD_DIR, safe_name)
    file.save(filepath)

    # 计算 SHA256
    sha256 = _sha256_file(filepath)
    file_size = os.path.getsize(filepath)

    models.firmware_add(version, safe_name, file_size, sha256, changelog, is_latest)

    return jsonify({"ok": True, "version": version, "sha256": sha256})


@app.route("/api/firmware/delete/<int:fw_id>", methods=["POST"])
@login_required
def api_delete(fw_id):
    models.firmware_delete(fw_id)
    return jsonify({"ok": True})


@app.route("/api/firmware/<int:fw_id>/set-latest", methods=["POST"])
@login_required
def api_set_latest(fw_id):
    models.firmware_set_latest(fw_id)
    return jsonify({"ok": True})


@app.route("/api/firmware/<int:fw_id>/push", methods=["POST"])
@login_required
def api_push_upgrade(fw_id):
    """推送升级命令给指定设备"""
    fw = models.firmware_get_by_id(fw_id)
    if not fw:
        return jsonify({"error": "firmware not found"}), 404

    device_id = request.form.get("device_id", "").strip()
    if not device_id:
        return jsonify({"error": "请指定设备ID"}), 400

    action = request.form.get("action", "upgrade")  # upgrade 或 downgrade
    success = push_upgrade(device_id, fw["version"], action)
    if success:
        return jsonify({"ok": True, "message": f"已向 {device_id} 发送 {action} 命令"})
    else:
        return jsonify({"error": "MQTT 推送失败，请检查 MQTT 连接"}), 500


# ============================================================
# 辅助函数
# ============================================================

def _version_greater(v1, v2):
    """比较版本号，v1 > v2 返回 True"""
    try:
        parts1 = [int(x) for x in v1.split(".")]
        parts2 = [int(x) for x in v2.split(".")]
        # 补齐长度
        while len(parts1) < 3: parts1.append(0)
        while len(parts2) < 3: parts2.append(0)
        for a, b in zip(parts1, parts2):
            if a > b: return True
            if a < b: return False
        return False  # 相等
    except ValueError:
        return v1 != v2  # 非标准版本号，简单字符串比较


def _sha256_file(filepath):
    sha = hashlib.sha256()
    with open(filepath, "rb") as f:
        while True:
            chunk = f.read(8192)
            if not chunk:
                break
            sha.update(chunk)
    return sha.hexdigest()


def _ts():
    return datetime.now().strftime("%Y%m%d_%H%M%S")


# ============================================================
# 启动
# ============================================================

if __name__ == "__main__":
    # 初始化
    os.makedirs(config.UPLOAD_DIR, exist_ok=True)
    models.init_db()

    # 启动 MQTT 客户端
    init_mqtt(
        config.MQTT_BROKER, config.MQTT_PORT,
        config.MQTT_USERNAME, config.MQTT_PASSWORD,
        config.MQTT_TOPIC_PREFIX
    )

    print(f"\n{'='*50}")
    print(f" OTA Server starting on http://0.0.0.0:{config.PORT}")
    print(f" Web UI:  http://<server-ip>:{config.PORT}")
    print(f" Login:   {config.ADMIN_USERNAME} / {config.ADMIN_PASSWORD}")
    print(f" API Key: {config.API_TOKEN}")
    print(f"{'='*50}\n")

    app.run(host=config.HOST, port=config.PORT, debug=True)
