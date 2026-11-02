#!/usr/bin/env bash
# 把 host/deck.py 装成 LaunchAgent：登录自启、崩了自动重启。
#
# 服务跑的是 ~/Library/Application Support/deck 下的副本，不是仓库里的文件：
# 改仓库里的代码不会把正在跑的服务弄坏，外部盘没挂载时也不受影响。
# 改完 host/ 下的代码后重新跑一次本脚本即可同步。
#
# 老项目的常驻服务（ai-quota-panel / agent-pond）会抢同一个串口，这里顺手停掉。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL="com.liaoxingyi.deck"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/deck.log"
DEST="$HOME/Library/Application Support/deck"
PYTHON="${PYTHON:-/opt/homebrew/bin/python3}"

for old in com.liaoxingyi.ai-quota-panel com.liaoxingyi.agent-pond; do
    if launchctl print "gui/$UID/$old" >/dev/null 2>&1; then
        launchctl bootout "gui/$UID/$old" 2>/dev/null || true
        echo "已停掉老服务 $old（plist 还在 ~/Library/LaunchAgents，确认不用了可以删）"
    fi
done

mkdir -p "$HOME/Library/LaunchAgents" "$DEST" "$(dirname "$LOG")"
rsync -a --delete --exclude '__pycache__' --exclude '.ruff_cache' "$ROOT/host/" "$DEST/"

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
        <string>$DEST/deck.py</string>
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
echo "  日志：tail -f $LOG"
echo "  停止：launchctl bootout gui/$UID/$LABEL"
echo "  卸载：launchctl bootout gui/$UID/$LABEL && rm $PLIST"
