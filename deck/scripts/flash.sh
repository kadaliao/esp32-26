#!/usr/bin/env bash
# 编译并烧录。直接运行即可（fish / zsh / bash 都行），不用先 source env.sh
# 常驻服务（install-agent.sh 装的）在跑的话会占着串口：烧录前先停，烧完再拉起来。
set -e
cd "$(dirname "$0")/.."
source scripts/env.sh >/dev/null
[ -n "$ESPPORT" ] || { echo "找不到板子：/dev/cu.usbmodem* 不存在，插上再试" >&2; exit 1; }

LABEL="com.liaoxingyi.deck"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
resume=0
if launchctl print "gui/$UID/$LABEL" >/dev/null 2>&1; then
    launchctl bootout "gui/$UID/$LABEL" 2>/dev/null || true
    resume=1
    echo "已暂停常驻服务 $LABEL，烧完会自动恢复"
fi
trap '[ "$resume" = 1 ] && launchctl bootstrap "gui/$UID" "$PLIST" && echo "常驻服务已恢复"' EXIT

idf.py -p "$ESPPORT" flash
