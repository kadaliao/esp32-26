#!/usr/bin/env bash
# 把 host/pond.py 装成 LaunchAgent：开机自启、崩了自动重启。
#
# 服务跑的是 ~/Library/Application Support 下的副本，不是仓库里的文件：
# 仓库在外部卷上，launchd 冷启动时从那里读文件会卡住（上个项目实测）。
# 改完 host/pond.py 后重跑本脚本同步。
#
# 串口只能有一个进程用。上个项目（AI 用量面板）的服务如果还在，会和这里抢口，
# 所以这里会把它停掉，并把它的 plist 改名为 .disabled（想恢复就改回来再 bootstrap）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL="com.liaoxingyi.agent-pond"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/agent-pond.log"
DEST_DIR="$HOME/Library/Application Support/agent-pond"
DEST="$DEST_DIR/pond.py"
PYTHON="${PYTHON:-/opt/homebrew/bin/python3}"

OLD_LABEL="com.liaoxingyi.ai-quota-panel"
OLD_PLIST="$HOME/Library/LaunchAgents/$OLD_LABEL.plist"
if [ -f "$OLD_PLIST" ]; then
    launchctl bootout "gui/$UID/$OLD_LABEL" 2>/dev/null || true
    mv "$OLD_PLIST" "$OLD_PLIST.disabled"
    echo "已停用旧服务 ${OLD_LABEL}（plist 改名为 .disabled）"
fi

mkdir -p "$HOME/Library/LaunchAgents" "$DEST_DIR" "$(dirname "$LOG")"
cp "$ROOT/host/pond.py" "$DEST"

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
        <string>/usr/bin:/bin:/usr/sbin:/sbin</string>
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
launchctl enable "gui/$UID/${LABEL}"

echo "已安装并启动：${LABEL}"
echo "  日志：$LOG"
echo "  停止：launchctl bootout gui/$UID/${LABEL}"
