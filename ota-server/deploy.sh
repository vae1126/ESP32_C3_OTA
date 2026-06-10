#!/bin/bash
# ============================================================
# OTA 固件管理平台 — 一键部署脚本（阿里云 ECS CentOS/Ubuntu）
# ============================================================
set -e

APP_DIR="/opt/ota-server"
PYTHON=$(which python3 || which python)

echo "===== OTA Server 一键部署 ====="
echo "目标目录: $APP_DIR"

# 1. 安装系统依赖
if command -v apt-get &>/dev/null; then
    sudo apt-get update -qq
    sudo apt-get install -y -qq python3 python3-pip python3-venv nginx
elif command -v yum &>/dev/null; then
    sudo yum install -y python3 python3-pip nginx
fi

# 2. 复制应用文件
sudo mkdir -p "$APP_DIR"
sudo cp -r . "$APP_DIR"
sudo chown -R $USER:$USER "$APP_DIR"

# 3. 创建 Python 虚拟环境并安装依赖
cd "$APP_DIR"
$PYTHON -m venv venv
source venv/bin/activate
pip install --upgrade pip -q
pip install -r requirements.txt -q
deactivate

# 4. 创建 uploads 目录
mkdir -p "$APP_DIR/uploads"

# 5. 配置 systemd 服务
sudo cp "$APP_DIR/ota-server.service" /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable ota-server
sudo systemctl restart ota-server

# 6. 配置 Nginx
NGINX_CONF="/etc/nginx/sites-enabled/ota-server"
NGINX_AVAILABLE="/etc/nginx/sites-available/ota-server"

sudo mkdir -p /etc/nginx/sites-{enabled,available}
sudo tee "$NGINX_AVAILABLE" > /dev/null <<'NGINX'
server {
    listen 80;
    server_name _;
    client_max_body_size 10M;
    location / {
        proxy_pass http://127.0.0.1:5000;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
NGINX

sudo ln -sf "$NGINX_AVAILABLE" "$NGINX_CONF" 2>/dev/null || sudo cp "$NGINX_AVAILABLE" "$NGINX_CONF"
sudo nginx -t && sudo systemctl reload nginx

echo ""
echo "===== 部署完成 ====="
echo "查看状态: sudo systemctl status ota-server"
echo "查看日志: sudo journalctl -u ota-server -f"
echo "Web 地址: http://$(hostname -I | awk '{print $1}')"
echo "默认账号: admin / admin123"
echo "===================="
