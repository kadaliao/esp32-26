#pragma once
#include <stdint.h>

// 4bpp 抗锯齿位图字体（scripts/gen-fonts.py 生成）
typedef struct {
    uint32_t off;      // 在 bmp 里的字节偏移
    uint8_t w, h;      // 位图尺寸
    int8_t xo, yo;     // 相对笔位置/行顶的偏移
    uint8_t adv;       // 前进宽度
} glyph_t;

typedef struct {
    const uint8_t *bmp;
    const glyph_t *g;
    uint8_t first, count;
    uint8_t line_h, ascent;
} font_t;

extern const font_t font_small;   // Avenir Next Medium 11
extern const font_t font_label;   // Avenir Next Demi Bold 13
extern const font_t font_serif;   // New York Italic 22
extern const font_t font_title;   // New York Italic 30
