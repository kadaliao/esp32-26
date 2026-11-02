#!/usr/bin/env bash
# 加载 ESP-IDF 环境（bash / fish 里都别直接 source，用 scripts/flash.sh）
#
# IDF 的位置按顺序找：
#   1. 已经设好的 $IDF_PATH
#   2. eim（ESP-IDF Installation Manager）装在 ~/.espressif/<ver>/esp-idf 的那套
#   3. 旧的 ~/workspace/esp32-playground/.tools/esp-idf-<ver>
# eim 的激活脚本会自己设好工具链和自带 venv 的 python，旧布局那套才需要 python3.14 shim
# （系统 python3 已经升到 3.15，而老 venv 是按 3.14 建的）。

IDFVER="${IDFVER:-v6.0.2}"
IDFVER="${IDFVER#esp-idf-}"          # 兼容 IDFVER=esp-idf-v6.0.2 的老写法

PLAYGROUND="${ESP32_PLAYGROUND:-$HOME/workspace/esp32-playground}"
ESPTOOLS="${IDF_TOOLS_PATH:-$HOME/.espressif/tools}"
EIM_IDF="$HOME/.espressif/$IDFVER/esp-idf"
LEGACY_IDF="$PLAYGROUND/.tools/esp-idf-$IDFVER"

export ESPPORT="${ESPPORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"

_activate=""
if [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/export.sh" ]; then
    _activate="already"
elif [ -f "$ESPTOOLS/activate_idf_$IDFVER.sh" ]; then
    _activate="$ESPTOOLS/activate_idf_$IDFVER.sh"
elif [ -f "$EIM_IDF/export.sh" ]; then
    _activate="$EIM_IDF/export.sh"
elif [ -f "$LEGACY_IDF/export.sh" ]; then
    _activate="$LEGACY_IDF/export.sh"
fi

# IDF 的激活脚本里有一堆探测命令会返回非零，先关掉 errexit，免得把 set -e 的调用方（flash.sh）带走
_errexit_on=0
case $- in *e*) _errexit_on=1; set +e ;; esac

case "$_activate" in
"")
    {
        echo "找不到 ESP-IDF $IDFVER，试过这些位置："
        echo "  \$IDF_PATH=${IDF_PATH:-<未设置>}"
        echo "  $ESPTOOLS/activate_idf_$IDFVER.sh"
        echo "  $EIM_IDF"
        echo "  $LEGACY_IDF"
        echo "装一套：https://dl.espressif.com/apps/eim/ ；已有 IDF 就 export IDF_PATH=<目录>"
    } >&2
    return 1 2>/dev/null || exit 1
    ;;
already)
    ;;
"$EIM_IDF/export.sh")
    export IDF_TOOLS_PATH="$ESPTOOLS"
    # shellcheck disable=SC1090
    source "$EIM_IDF/export.sh" >/dev/null 2>&1
    ;;
"$LEGACY_IDF/export.sh")
    _PY_SHIM="${TMPDIR:-/tmp}/py314-shim"
    mkdir -p "$_PY_SHIM"
    ln -sf /opt/homebrew/bin/python3.14 "$_PY_SHIM/python3"
    ln -sf /opt/homebrew/bin/python3.14 "$_PY_SHIM/python"
    export PATH="$_PY_SHIM:$PATH"
    export IDF_TOOLS_PATH="$PLAYGROUND/.tools/espressif"
    # shellcheck disable=SC1090
    source "$LEGACY_IDF/export.sh" >/dev/null 2>&1
    ;;
*)
    # eim 的激活脚本（activate_idf_<ver>.sh）。它用 ${0##*/} 判断"是不是被 source 的"，
    # 从 flash.sh 里 source 时它会以为自己是被直接执行的，于是 exit 1 把整个 shell 干掉。
    # 所以放进一个 $0=bash 的子 shell 里 source，再把改过的那批环境变量取回来。
    _env_dump="$(bash -c 'source "$1" >/dev/null 2>&1 && export -p' bash "$_activate" 2>/dev/null |
        grep -E '^declare -x (PATH|VIRTUAL_ENV|IDF_|ESP_|OPENOCD_SCRIPTS|PYTHONPATH)')"
    if [ -z "$_env_dump" ]; then
        echo "eim 激活脚本没跑通：$_activate" >&2
        return 1 2>/dev/null || exit 1
    fi
    eval "$_env_dump"
    # 这个脚本把 idf.py 定义成函数（venv/bin 里没有同名可执行文件），这里补一份
    if [ -n "${IDF_PATH:-}" ] && [ -n "${IDF_PYTHON_ENV_PATH:-}" ]; then
        idf.py() { "$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" "$@"; }
    fi
    ;;
esac
[ "$_errexit_on" = 1 ] && set -e

if ! command -v idf.py >/dev/null 2>&1; then
    echo "ESP-IDF 环境没加载成功（idf.py 不可用）" >&2
    return 1 2>/dev/null || exit 1
fi

echo "IDF ready: $(idf.py --version 2>/dev/null)  ESPPORT=${ESPPORT:-<未接入>}"
