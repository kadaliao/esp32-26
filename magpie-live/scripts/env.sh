#!/usr/bin/env bash
# 加载 ESP-IDF 环境（复用 esp32-playground 里已经装好的 IDF 6.0.2）
#
# 注意：系统 python3 已经升到 3.15，而 IDF 的 python 虚拟环境是按 3.14 建的。
# 这里临时把 python3 指向 3.14，否则 export.sh 会找不到 venv。

PLAYGROUND="${ESP32_PLAYGROUND:-$HOME/workspace/esp32-playground}"
IDFVER="${IDFVER:-esp-idf-v6.0.2}"

if [ ! -d "$PLAYGROUND/.tools/$IDFVER" ]; then
    echo "找不到 ESP-IDF: $PLAYGROUND/.tools/$IDFVER" >&2
    return 1 2>/dev/null || exit 1
fi

_PY_SHIM="${TMPDIR:-/tmp}/py314-shim"
mkdir -p "$_PY_SHIM"
ln -sf /opt/homebrew/bin/python3.14 "$_PY_SHIM/python3"
ln -sf /opt/homebrew/bin/python3.14 "$_PY_SHIM/python"

export PATH="$_PY_SHIM:$PATH"
export IDF_TOOLS_PATH="$PLAYGROUND/.tools/espressif"
export ESPPORT="${ESPPORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"

# shellcheck disable=SC1090
source "$PLAYGROUND/.tools/$IDFVER/export.sh" >/dev/null 2>&1

echo "IDF ready: $(idf.py --version 2>/dev/null)  ESPPORT=${ESPPORT:-<未接入>}"
