#!/usr/bin/env python3
"""把 macOS 系统字体栅格化成 4bpp 抗锯齿位图，生成 main/fonts.c。

用法：uv run --with pillow scripts/gen-fonts.py
字符集：ASCII 0x20-0x7E，外加 0x7F 槽位放 "·"。
字体只在这台 Mac 上生成一次，生成物已入库，编译不依赖本脚本。
"""
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = [
    # (C 名, 字体文件, face index, 像素大小)
    ("font_small", "/System/Library/Fonts/Avenir Next.ttc", 5, 11),   # Medium
    ("font_label", "/System/Library/Fonts/Avenir Next.ttc", 2, 13),   # Demi Bold
    ("font_serif", "/System/Library/Fonts/NewYorkItalic.ttf", 0, 22),
    ("font_title", "/System/Library/Fonts/NewYorkItalic.ttf", 0, 30),
]
CHARS = [chr(c) for c in range(0x20, 0x7F)] + ["·"]


def render(name, path, index, size):
    font = ImageFont.truetype(path, size, index=index)
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
