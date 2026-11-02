#include "gfx.h"

#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "gfx";

uint16_t *gfx_fb;
static uint8_t *s_trail;  // TRAIL_W * TRAIL_H * 3
static uint8_t s_bg[GFX_H][3];
static uint8_t s_water[GFX_H][3];

static const uint8_t BAYER[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

static void *alloc_fast(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_LOGW(TAG, "%u bytes fell back to PSRAM", (unsigned)n);
    }
    return p;
}

bool gfx_init(void)
{
    s_trail = alloc_fast(TRAIL_W * TRAIL_H * 3);
    gfx_fb = alloc_fast(GFX_W * GFX_H * 2);
    if (!s_trail || !gfx_fb) {
        return false;
    }
    memset(s_trail, 0, TRAIL_W * TRAIL_H * 3);
    memset(gfx_fb, 0, GFX_W * GFX_H * 2);
    ESP_LOGW(TAG, "internal free after gfx: %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

void gfx_set_bg_row(int y, uint8_t r, uint8_t g, uint8_t b)
{
    s_bg[y][0] = r;
    s_bg[y][1] = g;
    s_bg[y][2] = b;
}

void gfx_set_water_row(int y, uint8_t r, uint8_t g, uint8_t b)
{
    s_water[y][0] = r;
    s_water[y][1] = g;
    s_water[y][2] = b;
}

// ---------------------------------------------------------------- 颜色工具

static inline uint16_t pack(int r, int g, int b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline uint16_t pack_dither(int r, int g, int b, int d)
{
    r += d >> 1;
    g += d >> 2;
    b += d >> 1;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return pack(r, g, b);
}

static inline void unpack(uint16_t p, int *r, int *g, int *b)
{
    int r5 = p >> 11, g6 = (p >> 5) & 0x3F, b5 = p & 0x1F;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 2);
}

static inline int sat(int v)
{
    return v > 255 ? 255 : (v < 0 ? 0 : v);
}

// ---------------------------------------------------------------- 光层

void trail_fade(uint8_t keep)
{
    uint8_t *p = s_trail;
    for (int i = 0; i < TRAIL_W * TRAIL_H * 3; i++) {
        p[i] = (uint8_t)((p[i] * keep) >> 8);
    }
}

static inline void trail_add(int x, int y, int r, int g, int b)
{
    if ((unsigned)x >= TRAIL_W || (unsigned)y >= TRAIL_H) {
        return;
    }
    uint8_t *p = s_trail + (y * TRAIL_W + x) * 3;
    p[0] = (uint8_t)sat(p[0] + r);
    p[1] = (uint8_t)sat(p[1] + g);
    p[2] = (uint8_t)sat(p[2] + b);
}

void trail_glow(float x, float y, float radius, uint8_t r, uint8_t g, uint8_t b, float intensity)
{
    // 换到半分辨率坐标
    float hx = x * 0.5f - 0.25f, hy = y * 0.5f - 0.25f, hr = radius * 0.5f;
    if (hr < 0.5f || intensity <= 0.0f) {
        return;
    }
    int x0 = (int)floorf(hx - hr), x1 = (int)ceilf(hx + hr);
    int y0 = (int)floorf(hy - hr), y1 = (int)ceilf(hy + hr);
    float inv = 1.0f / (hr * hr);
    for (int yy = y0; yy <= y1; yy++) {
        for (int xx = x0; xx <= x1; xx++) {
            float dx = xx - hx, dy = yy - hy;
            float q = 1.0f - (dx * dx + dy * dy) * inv;
            if (q <= 0.0f) {
                continue;
            }
            float w = q * q * q * intensity;  // 近似高斯，边缘柔
            trail_add(xx, yy, (int)(r * w), (int)(g * w), (int)(b * w));
        }
    }
}

void trail_dot(float x, float y, uint8_t r, uint8_t g, uint8_t b, float intensity)
{
    float hx = x * 0.5f - 0.25f, hy = y * 0.5f - 0.25f;
    int ix = (int)floorf(hx), iy = (int)floorf(hy);
    float fx = hx - ix, fy = hy - iy;
    float w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    int ox[4] = {0, 1, 0, 1}, oy[4] = {0, 0, 1, 1};
    for (int k = 0; k < 4; k++) {
        float s = w[k] * intensity;
        trail_add(ix + ox[k], iy + oy[k], (int)(r * s), (int)(g * s), (int)(b * s));
    }
}

// ---------------------------------------------------------------- 合成

void gfx_compose(int rows)
{
    // 放大规则（每个半分辨率格子 c 对应整屏像素 2c、2c+1）：
    //   像素 2c   = (3*P[c] + P[c-1]) / 4
    //   像素 2c+1 = (3*P[c] + P[c+1]) / 4
    // 纵向同理。光层大部分是 0，整格为 0 时直接写预先抖动好的背景色。
    static int16_t row[(TRAIL_W + 2) * 3];
    static uint8_t lit[TRAIL_W + 2];
    if (rows > GFX_H) {
        rows = GFX_H;
    }
    for (int y = 0; y < rows; y++) {
        int k = y >> 1;
        int yb = (y & 1) ? (k + 1 < TRAIL_H ? k + 1 : k) : (k > 0 ? k - 1 : 0);
        const uint8_t *ra = s_trail + k * TRAIL_W * 3;
        const uint8_t *rb = s_trail + yb * TRAIL_W * 3;
        int16_t *rw = row + 3;  // 左右各留一格做边界
        for (int c = 0; c < TRAIL_W; c++) {
            int i = c * 3;
            int16_t r = (int16_t)(ra[i] * 3 + rb[i]);
            int16_t g = (int16_t)(ra[i + 1] * 3 + rb[i + 1]);
            int16_t b = (int16_t)(ra[i + 2] * 3 + rb[i + 2]);
            rw[i] = r;
            rw[i + 1] = g;
            rw[i + 2] = b;
            lit[c + 1] = (r | g | b) != 0;
        }
        memcpy(row, rw, 3 * sizeof(int16_t));                                   // 左边界 = 第 0 格
        memcpy(rw + TRAIL_W * 3, rw + (TRAIL_W - 1) * 3, 3 * sizeof(int16_t));  // 右边界
        lit[0] = lit[1];
        lit[TRAIL_W + 1] = lit[TRAIL_W];

        const uint8_t *bg = s_bg[y];
        const uint8_t *bay = BAYER[y & 3];
        uint16_t bgp[4];
        for (int j = 0; j < 4; j++) {
            bgp[j] = pack_dither(bg[0], bg[1], bg[2], bay[j]);
        }

        uint16_t *out = gfx_fb + y * GFX_W;
        for (int c = 0; c < TRAIL_W; c++) {
            int x = c * 2;
            if (!(lit[c] | lit[c + 1] | lit[c + 2])) {
                out[x] = bgp[x & 3];
                out[x + 1] = bgp[(x + 1) & 3];
                continue;
            }
            const int16_t *pl = rw + (c - 1) * 3, *pc = rw + c * 3, *pr = rw + (c + 1) * 3;
            int cr = pc[0] * 3, cg = pc[1] * 3, cb = pc[2] * 3;
            out[x] = pack_dither(bg[0] + ((cr + pl[0]) >> 4), bg[1] + ((cg + pl[1]) >> 4),
                                 bg[2] + ((cb + pl[2]) >> 4), bay[x & 3]);
            out[x + 1] = pack_dither(bg[0] + ((cr + pr[0]) >> 4), bg[1] + ((cg + pr[1]) >> 4),
                                     bg[2] + ((cb + pr[2]) >> 4), bay[(x + 1) & 3]);
        }
    }
}

// ---------------------------------------------------------------- fb 绘制

void fb_add(int x, int y, int r, int g, int b)
{
    if ((unsigned)x >= GFX_W || (unsigned)y >= GFX_H) {
        return;
    }
    uint16_t *p = gfx_fb + y * GFX_W + x;
    int cr, cg, cb;
    unpack(*p, &cr, &cg, &cb);
    *p = pack(sat(cr + r), sat(cg + g), sat(cb + b));
}

void fb_blend(int x, int y, uint8_t r, uint8_t g, uint8_t b, int a)
{
    if ((unsigned)x >= GFX_W || (unsigned)y >= GFX_H || a <= 0) {
        return;
    }
    if (a > 255) {
        a = 255;
    }
    uint16_t *p = gfx_fb + y * GFX_W + x;
    int cr, cg, cb;
    unpack(*p, &cr, &cg, &cb);
    cr += ((r - cr) * a) / 255;
    cg += ((g - cg) * a) / 255;
    cb += ((b - cb) * a) / 255;
    *p = pack(cr, cg, cb);
}

void fb_disc(float cx, float cy, float rad, uint8_t r, uint8_t g, uint8_t b, float a)
{
    int x0 = (int)floorf(cx - rad - 1), x1 = (int)ceilf(cx + rad + 1);
    int y0 = (int)floorf(cy - rad - 1), y1 = (int)ceilf(cy + rad + 1);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float cov = rad + 0.5f - sqrtf(dx * dx + dy * dy);
            if (cov <= 0.0f) {
                continue;
            }
            if (cov > 1.0f) {
                cov = 1.0f;
            }
            float s = cov * a;
            fb_add(x, y, (int)(r * s), (int)(g * s), (int)(b * s));
        }
    }
}

void fb_ring(float cx, float cy, float rad, float width, uint8_t r, uint8_t g, uint8_t b, float a)
{
    float outer = rad + width;
    int x0 = (int)floorf(cx - outer - 1), x1 = (int)ceilf(cx + outer + 1);
    int y0 = (int)floorf(cy - outer - 1), y1 = (int)ceilf(cy + outer + 1);
    float hw = width * 0.5f;
    float mid = rad + hw;
    for (int y = y0; y <= y1; y++) {
        float dy = y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx;
            float d = fabsf(sqrtf(dx * dx + dy * dy) - mid);
            float cov = hw + 0.5f - d;
            if (cov <= 0.0f) {
                continue;
            }
            if (cov > 1.0f) {
                cov = 1.0f;
            }
            float s = cov * a;
            fb_add(x, y, (int)(r * s), (int)(g * s), (int)(b * s));
        }
    }
}

// ---------------------------------------------------------------- 倒影

void gfx_reflect(int wy, float t)
{
    for (int y = wy; y < GFX_H; y++) {
        int d = y - wy + 1;
        int sy = wy - (d * 5) / 2;  // 纵向压扁 2.5 倍：水面能映出大半个天空
        if (sy < 0) {
            sy = 0;
        }
        // 越深越暗、波纹越大
        int k = 140 - d * 2;  // /256
        if (k < 30) {
            k = 30;
        }
        int off = (int)lrintf(sinf(d * 0.55f - t * 2.1f) * (0.6f + d * 0.06f) +
                              sinf(d * 1.7f + t * 1.3f) * 0.5f);
        const uint16_t *src = gfx_fb + sy * GFX_W;
        uint16_t *dst = gfx_fb + y * GFX_W;
        const uint8_t *wc = s_water[y];
        const uint8_t *bay = BAYER[y & 3];
        for (int x = 0; x < GFX_W; x++) {
            int sx = x + off;
            sx = sx < 0 ? 0 : (sx >= GFX_W ? GFX_W - 1 : sx);
            int r, g, b;
            unpack(src[sx], &r, &g, &b);
            dst[x] = pack_dither(wc[0] + ((r * k) >> 8), wc[1] + ((g * k) >> 8),
                                 wc[2] + ((b * k) >> 8) + ((b * k) >> 10), bay[x & 3]);
        }
    }
}

// ---------------------------------------------------------------- 文字

static const glyph_t *glyph(const font_t *f, char ch)
{
    unsigned c = (unsigned char)ch;
    if (c < f->first || c >= (unsigned)(f->first + f->count)) {
        c = '?';
    }
    return &f->g[c - f->first];
}

int text_width(const font_t *f, const char *s)
{
    int w = 0;
    for (; *s; s++) {
        w += glyph(f, *s)->adv;
    }
    return w;
}

static void draw_glyphs(const font_t *f, int x, int y, const char *s, uint8_t r, uint8_t g,
                        uint8_t b, int a)
{
    for (; *s; s++) {
        const glyph_t *gl = glyph(f, *s);
        const uint8_t *bmp = f->bmp + gl->off;
        int n = 0;
        for (int yy = 0; yy < gl->h; yy++) {
            for (int xx = 0; xx < gl->w; xx++, n++) {
                int v = (n & 1) ? (bmp[n >> 1] & 0x0F) : (bmp[n >> 1] >> 4);
                if (v) {
                    fb_blend(x + gl->xo + xx, y + gl->yo + yy, r, g, b, (v * 17 * a) / 255);
                }
            }
        }
        x += gl->adv;
    }
}

void text_draw(const font_t *f, int x, int y, const char *s, uint8_t r, uint8_t g, uint8_t b, int a)
{
    draw_glyphs(f, x + 1, y + 1, s, 0, 0, 8, a * 2 / 3);
    draw_glyphs(f, x, y, s, r, g, b, a);
}

// ---------------------------------------------------------------- 覆盖式圆（喜鹊的黑身子要能盖住光）

void fb_bdisc(float cx, float cy, float rad, uint8_t r, uint8_t g, uint8_t b, float a)
{
    int x0 = (int)floorf(cx - rad - 1), x1 = (int)ceilf(cx + rad + 1);
    int y0 = (int)floorf(cy - rad - 1), y1 = (int)ceilf(cy + rad + 1);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float cov = rad + 0.5f - sqrtf(dx * dx + dy * dy);
            if (cov <= 0.0f) {
                continue;
            }
            if (cov > 1.0f) {
                cov = 1.0f;
            }
            fb_blend(x, y, r, g, b, (int)(255 * cov * a));
        }
    }
}

// 粗细可调的抗锯齿线段（用一串覆盖式圆点近似，够短的线足够好）
void fb_bline(float x0, float y0, float x1, float y1, float w, uint8_t r, uint8_t g, uint8_t b, float a)
{
    float dx = x1 - x0, dy = y1 - y0;
    int n = (int)(sqrtf(dx * dx + dy * dy) * 1.5f) + 1;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / n;
        fb_bdisc(x0 + dx * t, y0 + dy * t, w * 0.5f, r, g, b, a);
    }
}

// ---------------------------------------------------------------- 贴图

void fb_image(int x0, int y0, const image_t *img, float a)
{
    for (int y = 0; y < img->h; y++) {
        for (int x = 0; x < img->w; x++) {
            int i = y * img->w + x;
            int al = (int)(img->a[i] * a);
            if (al <= 0) continue;
            uint16_t p = img->px[i];
            fb_blend(x0 + x, y0 + y, (uint8_t)((p >> 11) * 255 / 31), (uint8_t)(((p >> 5) & 63) * 255 / 63),
                     (uint8_t)((p & 31) * 255 / 31), al);
        }
    }
}
