# Claude Code 会话同步说明

将此仓库克隆到另一台电脑后，执行以下命令恢复 Claude Code 上下文：

## Windows (PowerShell)
```powershell
# 1. 克隆仓库
git clone https://github.com/vae1126/ESP32_C3_OTA.git
cd ESP32_C3_OTA

# 2. 复制 Claude Code 状态文件到用户目录
$src = "$PWD\.claude"
$dst = "$env:USERPROFILE\.claude"
Copy-Item "$src\settings.json" "$dst\" -Force
Copy-Item "$src\plans" "$dst\plans" -Recurse -Force
Copy-Item "$src\settings.local.json" "$dst\projects\c--Users-HS-Desktop-esp32c3-home-3-0-0\" -Force

# 3. 在项目目录启动 Claude Code
claude
```

## macOS / Linux
```bash
git clone https://github.com/vae1126/ESP32_C3_OTA.git
cd ESP32_C3_OTA
cp .claude/settings.json ~/.claude/
cp -r .claude/plans ~/.claude/
mkdir -p ~/.claude/projects/c--Users-HS-Desktop-esp32c3-home-3-0-0/
cp .claude/settings.local.json ~/.claude/projects/c--Users-HS-Desktop-esp32c3-home-3-0-0/
```

> 注意：路径 `c--Users-HS-Desktop-esp32c3-home-3-0-0` 是当前电脑的项目路径编码名，
> 另一台电脑上的路径可能不同，启动 Claude Code 后会自动创建对应目录。
