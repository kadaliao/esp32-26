#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "font.h"
#include "logo.h"

// 极简软件渲染器，所有 app 共用。两层：
//   fb     整屏 RGB565（小端，和截图格式一致），最终画面
//   trail  半分辨率 RGB888 光层：光晕和火星画在这里，每帧按比例衰减，
//          于是运动的光会自然拖出彗尾；合成时双线性放大，所以光晕是柔的
#define GFX_W  320
#define GFX_H  240
#define TRAIL_W (GFX_W / 2)
#define TRAIL_H (GFX_H / 2)

extern uint16_t *gfx_fb;

bool gfx_init(void);

// 背景渐变：每行一种底色（app 进场时填，因为是全局共用的）
void gfx_set_bg_row(int y, uint8_t r, uint8_t g, uint8_t b);

// ---- 光层 ----
void trail_fade(uint8_t keep);  // 每个通道乘 keep/256
void trail_clear(void);         // 切 app 时清掉上一个 app 的余光
// 柔光斑：x,y,radius 都是整屏坐标；intensity 0..1
void trail_glow(float x, float y, float radius, uint8_t r, uint8_t g, uint8_t b, float intensity);
// 亚像素光点（双线性摊到 4 个格子）
void trail_dot(float x, float y, uint8_t r, uint8_t g, uint8_t b, float intensity);

// 背景 + 光层 合成到 fb 的 [0, rows) 行，带 4x4 有序抖动
void gfx_compose(int rows);

// ---- 直接画到 fb ----
void fb_add(int x, int y, int r, int g, int b);                 // 加色
void fb_blend(int x, int y, uint8_t r, uint8_t g, uint8_t b, int a);  // a: 0..255
void fb_disc(float cx, float cy, float rad, uint8_t r, uint8_t g, uint8_t b, float a);  // 抗锯齿实心圆（加色）
void fb_ring(float cx, float cy, float rad, float width, uint8_t r, uint8_t g, uint8_t b, float a);

void fb_bdisc(float cx, float cy, float rad, uint8_t r, uint8_t g, uint8_t b, float a);  // 覆盖式
void fb_bline(float x0, float y0, float x1, float y1, float w, uint8_t r, uint8_t g, uint8_t b, float a);

void fb_image(int x, int y, const image_t *img, float a);  // 左上角坐标，a: 0..1
// 单通道遮罩（A8 logo）按指定颜色着色贴上去
void fb_mask(int x, int y, int w, int h, const uint8_t *alpha, uint8_t r, uint8_t g, uint8_t b, float a);

void fb_fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, int a);  // 覆盖式矩形，a: 0..255
void fb_fill_add(int x, int y, int w, int h, int r, int g, int b);                 // 加色矩形
void fb_frame(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, int a); // 1px 边框
void fb_brackets(int x, int y, int w, int h, int len, uint8_t r, uint8_t g, uint8_t b, int a);  // HUD 四角括号
void fb_rbox(int x, int y, int w, int h, int rad, uint8_t r, uint8_t g, uint8_t b, int a);      // 实心圆角矩形

// 水面倒影：把 wy 以上的画面镜像到 wy 以下，带波纹与水色
void gfx_reflect(int wy, float t);
void gfx_set_water_row(int y, uint8_t r, uint8_t g, uint8_t b);

// ---- 文字（UTF-8；字库外的字符画成 ?）----
enum { ALIGN_LEFT = 0, ALIGN_CENTER, ALIGN_RIGHT };

int text_width(const font_t *f, const char *s);
int text_width_sp(const font_t *f, const char *s, int sp);  // sp: 额外字距
// y 是行顶；a: 0..255；带一像素暗影便于压在光晕上读
void text_draw(const font_t *f, int x, int y, const char *s, uint8_t r, uint8_t g, uint8_t b, int a);
// 不带暗影，可加字距
void text_plain(const font_t *f, int x, int y, const char *s, int sp, uint8_t r, uint8_t g, uint8_t b, int a);
// 按 align 对齐到 x（不带暗影），返回文字宽度
int text_align(const font_t *f, int x, int y, int align, const char *s, int sp, uint8_t r, uint8_t g,
               uint8_t b, int a);
// 太宽就截断并补 "…"（原地改，cap 是 s 的缓冲区大小）
void text_fit(const font_t *f, char *s, size_t cap, int maxw);
