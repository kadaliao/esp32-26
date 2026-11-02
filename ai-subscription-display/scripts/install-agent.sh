#!/usr/bin/env bash
# 把 bridge.py 装成 LaunchAgent：开机自启、崩了自动重启。
#
# 注意：服务跑的是 ~/Library/Application Support 下的**副本**，不是仓库里的文件。
# 因为仓库在外部卷 /Volumes/Data-External 上，launchd 冷启动时从那里 import
# 会卡在 open() 上（实测），而且外部盘没挂载时服务也不该挂。
# 改完 host/bridge.py 后重新跑一次本脚本即可同步。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL="com.liaoxingyi.ai-quota-panel"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/ai-quota-panel.log"
DEST_DIR="$HOME/Library/Application Support/ai-quota-panel"
DEST="$DEST_DIR/bridge.py"
PYTHON="${PYTHON:-/opt/homebrew/bin/python3}"

mkdir -p "$HOME/Library/LaunchAgents" "$DEST_DIR" "$(dirname "$LOG")"
cp "$ROOT/host/bridge.py" "$DEST"
chmod +x "$DEST"

cat > "$PLIST" <<PLISTEOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$PYTHON</string>
        <string>-u</string>
        <string>$DEST</string>
    </array>
    <key>WorkingDirectory</key>
    <string>$HOME</string>
    <key>EnvironmentVariables</key>
    <dict>
        <key>PATH</key>
        <string>/opt/homebrew/bin:/usr/local/bin:$HOME/.local/bin:/usr/bin:/bin:/usr/sbin:/sbin</string>
        <key>HOME</key>
        <string>$HOME</string>
    </dict>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>$LOG</string>
    <key>StandardErrorPath</key>
    <string>$LOG</string>
</dict>
</plist>
PLISTEOF

launchctl bootout "gui/$UID/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$UID" "$PLIST"
launchctl enable "gui/$UID/$LABEL"

echo "已安装并启动：$LABEL"
echo "  运行副本：$DEST"
echo "  日志：$LOG"
echo "  停止：launchctl bootout gui/$UID/$LABEL"
echo "  卸载：launchctl bootout gui/$UID/$LABEL && rm $PLIST"
