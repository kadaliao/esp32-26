#!/usr/bin/env python3
"""把 magpie.app 的图标转成固件里的 RGB565 + alpha 位图（main/logo.c）。

用法：uv run --with pillow python scripts/gen-logo.py [icns 路径]
"""
import subprocess, sys, tempfile, os
from PIL import Image

src = sys.argv[1] if len(sys.argv) > 1 else "/Applications/magpie.app/Contents/Resources/magpie.icns"
SIZES = {"logo_s": 20, "logo_m": 30, "logo_l": 72}

with tempfile.TemporaryDirectory() as d:
    png = os.path.join(d, "icon.png")
    subprocess.run(["sips", "-s", "format", "png", "-z", "512", "512", src, "--out", png],
                   check=True, capture_output=True)
    big = Image.open(png).convert("RGBA")

# 图标四周有透明边，先裁到圆角底的范围
big = big.crop(big.getbbox())

out = ['// 由 scripts/gen-logo.py 生成，别手改', '#include "logo.h"', '']
for name, n in SIZES.items():
    im = big.resize((n, n), Image.LANCZOS)
    px, al = [], []
    for r, g, b, a in im.get_flattened_data():
        px.append(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
        al.append(a)
    out.append(f"static const uint16_t {name}_px[] = {{" + ",".join(map(str, px)) + "};")
    out.append(f"static const uint8_t {name}_a[] = {{" + ",".join(map(str, al)) + "};")
    out.append(f"const image_t {name} = {{{n}, {n}, {name}_px, {name}_a}};")
    out.append("")
open(os.path.join(os.path.dirname(__file__), "..", "main", "logo.c"), "w").write("\n".join(out))
print("ok", list(SIZES.values()))
