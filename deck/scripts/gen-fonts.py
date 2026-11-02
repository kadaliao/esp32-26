#!/usr/bin/env python3
"""把 macOS 系统字体栅格化成 4bpp 抗锯齿位图，生成 main/fonts.c。

用法：uv run --with pillow scripts/gen-fonts.py
字符集：ASCII 0x20-0x7E，后面接几个扩展槽位（EXTRA），固件里 text_* 会把 UTF-8 映射过来。
字体只在这台 Mac 上生成一次，生成物已入库，编译不依赖本脚本。
"""
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AVENIR = "/System/Library/Fonts/Avenir Next.ttc"
AVENIR_C = "/System/Library/Fonts/Avenir Next Condensed.ttc"
SF_MONO = "/System/Library/Fonts/SFNSMono.ttf"
FONTS = [
    # (C 名, 字体文件, face index 或可变字体的字重名, 像素大小)
    ("font_small", AVENIR, 5, 11),            # Medium
    ("font_label", AVENIR, 2, 13),            # Demi Bold
    ("font_serif", "/System/Library/Fonts/NewYorkItalic.ttf", 0, 22),
    ("font_title", "/System/Library/Fonts/NewYorkItalic.ttf", 0, 30),
    ("font_mono", SF_MONO, "Medium", 11),     # HUD 数字、进程表
    ("font_tiny", SF_MONO, "Semibold", 9),    # 字距拉开的小标签
    ("font_num", AVENIR_C, 2, 18),            # 中号数字
    ("font_big", AVENIR_C, 2, 32),            # 大号数字
]
# 扩展槽位：0x7F 起依次排；顺序要和 gfx.c 里 map_cp() 一致
EXTRA = ["·", "¥", "°", "…"]
CHARS = [chr(c) for c in range(0x20, 0x7F)] + EXTRA


def load(path, face, size):
    if isinstance(face, str):  # 可变字体按字重名取
        font = ImageFont.truetype(path, size)
        font.set_variation_by_name(face)
        return font
    return ImageFont.truetype(path, size, index=face)


def render(name, path, face, size):
    font = load(path, face, size)
    ascent, descent = font.getmetrics()
    glyphs, data = [], bytearray()
    for ch in CHARS:
        adv = round(font.getlength(ch))
        box = font.getbbox(ch)
        x0, y0, x1, y1 = box
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        if w == 0 or h == 0:
            glyphs.append((len(data), 0, 0, 0, 0, adv))
            continue
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255)
        px = img.load()
        off = len(data)
        nib = []
        for y in range(h):
            for x in range(w):
                nib.append((px[x, y] + 8) // 17)  # 0..15
        if len(nib) % 2:
            nib.append(0)
        for i in range(0, len(nib), 2):
            data.append((nib[i] << 4) | nib[i + 1])
        glyphs.append((off, w, h, x0, y0, adv))
    lines = [f"static const uint8_t {name}_bmp[] = {{"]
    for i in range(0, len(data), 24):
        lines.append("    " + ",".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
    lines.append("};")
    lines.append(f"static const glyph_t {name}_glyphs[] = {{")
    for g in glyphs:
        lines.append("    {%d,%d,%d,%d,%d,%d}," % g)
    lines.append("};")
    lines.append(f"const font_t {name} = {{{name}_bmp, {name}_glyphs, 0x20, {len(CHARS)}, "
                 f"{ascent + descent}, {ascent}}};")
    return "\n".join(lines), len(data)


def main():
    out = ["// 由 scripts/gen-fonts.py 生成，勿手改", '#include "font.h"', ""]
    for spec in FONTS:
        code, n = render(*spec)
        out.append(code)
        out.append("")
        print(f"{spec[0]}: {n} bytes")
    with open(os.path.join(ROOT, "main", "fonts.c"), "w") as fh:
        fh.write("\n".join(out))


if __name__ == "__main__":
    main()
