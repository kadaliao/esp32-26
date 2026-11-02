#!/usr/bin/env bash
# 编译并烧录。直接运行即可（fish / zsh / bash 都行），不用先 source env.sh
set -e
cd "$(dirname "$0")/.."
source scripts/env.sh >/dev/null
[ -n "$ESPPORT" ] || { echo "找不到板子：/dev/cu.usbmodem* 不存在，插上再试" >&2; exit 1; }
idf.py -p "$ESPPORT" flash
