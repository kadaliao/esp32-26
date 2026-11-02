#!/usr/bin/env bash
# 把 assets/logos/*.svg 转成 LVGL 的 A8（纯透明度）图片，写入 main/logos.c。
# A8 只存形状，颜色在运行时用 image_recolor 着色，所以每个 logo 只要 SIZE*SIZE 字节。
# 依赖：rsvg-convert（librsvg）、magick（ImageMagick）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SIZE="${SIZE:-16}"
OUT="$ROOT/main/logos.c"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

{
    echo "// 由 scripts/gen-logos.sh 生成，不要手改。源：assets/logos/*.svg"
    echo '#include "logos.h"'
    echo
    for svg in "$ROOT"/assets/logos/*.svg; do
        name="$(basename "$svg" .svg)"
        # 先 4 倍超采样再缩小，边缘抗锯齿更干净
        rsvg-convert -w $((SIZE * 4)) -h $((SIZE * 4)) "$svg" -o "$TMP/$name.png"
        magick "$TMP/$name.png" -alpha extract -resize "${SIZE}x${SIZE}" -depth 8 "gray:$TMP/$name.a8"
        echo "static const uint8_t ${name}_map[] = {"
        xxd -i < "$TMP/$name.a8"
        echo "};"
        echo
        echo "const lv_image_dsc_t logo_${name} = {"
        echo "    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,"
        echo "               .w = ${SIZE}, .h = ${SIZE}, .stride = ${SIZE}},"
        echo "    .data_size = sizeof(${name}_map),"
        echo "    .data = ${name}_map,"
        echo "};"
        echo
    done
} > "$OUT"

echo "已生成 ${OUT}（${SIZE}x${SIZE} A8）"
