#include "gfx.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "gfx";

uint16_t *gfx_fb;
static uint8_t *s_trail;  // TRAIL_W * TRAIL_H * 3
static uint8_t s_row_lit[TRAIL_H];  // 光层这一行有没有不为 0 的格子，合成时整行跳过暗的
static int16_t s_row_x0[TRAIL_H], s_row_x1[TRAIL_H];  // 有光的那一段 [x0, x1]（格子），衰减和合成只管这段
static uint8_t s_bg[GFX_H][3];
static uint8_t s_water[GFX_H][3];

// 合成用的查表：下标 = 底色 + 光 + 抖动（最大 255+255+7），直接给出钳好、移好位的 565 分量
#define LUT_N 528
static uint16_t s_lut_r[LUT_N], s_lut_g[LUT_N], s_lut_b[LUT_N];

static const uint8_t BAYER[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

static void cjk_init(void);

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
    cjk_init();
    for (int v = 0; v < LUT_N; v++) {
        int c = v > 255 ? 255 : v;
        s_lut_r[v] = (uint16_t)((c >> 3) << 11);
        s_lut_g[v] = (uint16_t)((c >> 2) << 5);
        s_lut_b[v] = (uint16_t)(c >> 3);
    }
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
    for (int y = 0; y < TRAIL_H; y++) {
        if (!s_row_lit[y]) {
            continue;
        }
        uint8_t *p = s_trail + y * TRAIL_W * 3;
        int x0 = -1, x1 = -1;
        for (int c = s_row_x0[y]; c <= s_row_x1[y]; c++) {
            uint8_t *q = p + c * 3;
            q[0] = (uint8_t)((q[0] * keep) >> 8);
            q[1] = (uint8_t)((q[1] * keep) >> 8);
            q[2] = (uint8_t)((q[2] * keep) >> 8);
            if (q[0] | q[1] | q[2]) {
                if (x0 < 0) x0 = c;
                x1 = c;
            }
        }
        s_row_lit[y] = x0 >= 0;
        s_row_x0[y] = (int16_t)x0;
        s_row_x1[y] = (int16_t)x1;
    }
}

static inline void trail_add(int x, int y, int r, int g, int b)
{
    if ((unsigned)x >= TRAIL_W || (unsigned)y >= TRAIL_H) {
        return;
    }
    uint8_t *p = s_trail + (y * TRAIL_W + x) * 3;
    if (!s_row_lit[y]) {
        s_row_lit[y] = 1;
        s_row_x0[y] = s_row_x1[y] = (int16_t)x;
    } else if (x < s_row_x0[y]) {
        s_row_x0[y] = (int16_t)x;
    } else if (x > s_row_x1[y]) {
        s_row_x1[y] = (int16_t)x;
    }
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
        if (!s_row_lit[k] && !s_row_lit[yb]) {
            // 整行没光：直接铺预先抖动好的背景色
            const uint8_t *bg = s_bg[y];
            const uint8_t *bay = BAYER[y & 3];
            uint16_t bgp[4];
            for (int j = 0; j < 4; j++) {
                bgp[j] = pack_dither(bg[0], bg[1], bg[2], bay[j]);
            }
            uint16_t *out = gfx_fb + y * GFX_W;
            for (int x = 0; x < GFX_W; x += 4) {
                out[x] = bgp[0];
                out[x + 1] = bgp[1];
                out[x + 2] = bgp[2];
                out[x + 3] = bgp[3];
            }
            continue;
        }
        const uint8_t *ra = s_trail + k * TRAIL_W * 3;
        const uint8_t *rb = s_trail + yb * TRAIL_W * 3;
        // 这两行光层有光的范围，左右各多算一格（插值要用到邻格）；范围外直接铺背景
        int a0 = s_row_lit[k] ? s_row_x0[k] : TRAIL_W, a1 = s_row_lit[k] ? s_row_x1[k] : -1;
        if (s_row_lit[yb]) {
            if (s_row_x0[yb] < a0) a0 = s_row_x0[yb];
            if (s_row_x1[yb] > a1) a1 = s_row_x1[yb];
        }
        int c0 = a0 > 0 ? a0 - 1 : 0, c1 = a1 < TRAIL_W - 1 ? a1 + 1 : TRAIL_W - 1;
        int16_t *rw = row + 3;  // 左右各留一格做边界
        for (int c = c0; c <= c1; c++) {
            int i = c * 3;
            int16_t r = (int16_t)(ra[i] * 3 + rb[i]);
            int16_t g = (int16_t)(ra[i + 1] * 3 + rb[i + 1]);
            int16_t b = (int16_t)(ra[i + 2] * 3 + rb[i + 2]);
            rw[i] = r;
            rw[i + 1] = g;
            rw[i + 2] = b;
            lit[c + 1] = (r | g | b) != 0;
        }
        if (c0 > 0) {  // 范围外的邻格是 0
            memset(rw + (c0 - 1) * 3, 0, 3 * sizeof(int16_t));
            lit[c0] = 0;
        } else {       // 左边界 = 第 0 格
            memcpy(row, rw, 3 * sizeof(int16_t));
            lit[0] = lit[1];
        }
        if (c1 < TRAIL_W - 1) {
            memset(rw + (c1 + 1) * 3, 0, 3 * sizeof(int16_t));
            lit[c1 + 2] = 0;
        } else {       // 右边界 = 最后一格
            memcpy(rw + TRAIL_W * 3, rw + (TRAIL_W - 1) * 3, 3 * sizeof(int16_t));
            lit[TRAIL_W + 1] = lit[TRAIL_W];
        }

        const uint8_t *bg = s_bg[y];
        const uint8_t *bay = BAYER[y & 3];
        uint16_t bgp[4];
        int br[4], bgg[4], bb[4];  // 底色 + 这一列的抖动量，和 pack_dither 的加法一致
        for (int j = 0; j < 4; j++) {
            bgp[j] = pack_dither(bg[0], bg[1], bg[2], bay[j]);
            br[j] = bg[0] + (bay[j] >> 1);
            bgg[j] = bg[1] + (bay[j] >> 2);
            bb[j] = bg[2] + (bay[j] >> 1);
        }

        uint16_t *out = gfx_fb + y * GFX_W;
        for (int x = 0; x < c0 * 2; x++) {
            out[x] = bgp[x & 3];
        }
        for (int x = (c1 + 1) * 2; x < GFX_W; x++) {
            out[x] = bgp[x & 3];
        }
        for (int c = c0; c <= c1; c++) {
            int x = c * 2;
            int j = x & 3;  // x 是偶数，所以 j 是 0 或 2，右边那个像素是 j+1
            if (!(lit[c] | lit[c + 1] | lit[c + 2])) {
                out[x] = bgp[j];
                out[x + 1] = bgp[j + 1];
                continue;
            }
            const int16_t *pl = rw + (c - 1) * 3, *pc = rw + c * 3, *pr = rw + (c + 1) * 3;
            int cr = pc[0] * 3, cg = pc[1] * 3, cb = pc[2] * 3;
            out[x] = s_lut_r[br[j] + ((cr + pl[0]) >> 4)] | s_lut_g[bgg[j] + ((cg + pl[1]) >> 4)] |
                     s_lut_b[bb[j] + ((cb + pl[2]) >> 4)];
            out[x + 1] = s_lut_r[br[j + 1] + ((cr + pr[0]) >> 4)] | s_lut_g[bgg[j + 1] + ((cg + pr[1]) >> 4)] |
                         s_lut_b[bb[j + 1] + ((cb + pr[2]) >> 4)];
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

// RGB565 直接混色：把 565 摊开成 0x07E0F81F 的 32 位格式，三个通道一次乘完。a5: 0..32
static inline uint16_t blend565(uint16_t dst, uint16_t src, int a5)
{
    uint32_t d = (dst | ((uint32_t)dst << 16)) & 0x07E0F81Fu;
    uint32_t c = (src | ((uint32_t)src << 16)) & 0x07E0F81Fu;
    uint32_t m = ((d * (uint32_t)(32 - a5) + c * (uint32_t)a5) >> 5) & 0x07E0F81Fu;
    return (uint16_t)(m | (m >> 16));
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
    *p = blend565(*p, pack(r, g, b), (a + 4) >> 3);
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
        int wr[4], wg[4], wb[4];  // 水色 + 这一列的抖动量
        for (int j = 0; j < 4; j++) {
            wr[j] = wc[0] + (bay[j] >> 1);
            wg[j] = wc[1] + (bay[j] >> 2);
            wb[j] = wc[2] + (bay[j] >> 1);
        }
        for (int x = 0; x < GFX_W; x++) {
            int sx = x + off;
            sx = sx < 0 ? 0 : (sx >= GFX_W ? GFX_W - 1 : sx);
            // 直接拿 5/6 位分量乘：r5*k>>5 约等于 r8*k>>8，省掉展开成 8 位再打包
            uint16_t p = src[sx];
            int rr = ((p >> 11) * k) >> 5, gg = (((p >> 5) & 63) * k) >> 6, bb = ((p & 31) * k) >> 5;
            int j = x & 3;
            dst[x] = s_lut_r[wr[j] + rr] | s_lut_g[wg[j] + gg] | s_lut_b[wb[j] + bb + (bb >> 2)];
        }
    }
}

// ---------------------------------------------------------------- 文字

// UTF-8 → 主字库槽位。ASCII 原样；扩展字符的顺序和 scripts/gen-fonts.py 的 EXTRA 一致；
// 主字库没有的返回 -1，去中文后备字库里找
static int map_cp(int cp)
{
    switch (cp) {
    case 0xB7: return 0x7F;    // ·
    case 0xA5: return 0x80;    // ¥
    case 0xB0: return 0x81;    // °
    case 0x2026: return 0x82;  // …
    default: return cp < 0x80 ? cp : -1;
    }
}

// 解出下一个字符的码位；坏的序列整段跳过，记一个问号
static int next_char(const char **ps)
{
    const unsigned char *p = (const unsigned char *)*ps;
    if (p[0] < 0x80) {
        *ps += 1;
        return p[0];
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *ps += 2;
        return ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *ps += 3;
        return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    int n = (p[0] & 0xF8) == 0xF0 ? 4 : 1;  // 4 字节序列（emoji 之类）或者被截断的序列
    for (int i = 1; i < n && p[i]; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            n = i;
            break;
        }
    }
    *ps += n;
    return '?';
}

// ---- 中文后备字库：scripts/gen-fonts.py 生成的 cjk12.bin，原样嵌在 flash 里 ----
extern const uint8_t cjk_bin_start[] asm("_binary_cjk12_bin_start");

static struct {
    int n;
    uint8_t ascent;
    const uint8_t *cps;   // u16 × n，升序
    const uint8_t *recs;  // 10 字节 × n
    const uint8_t *bmp;
} s_cjk;

static void cjk_init(void)
{
    const uint8_t *p = cjk_bin_start;
    if (memcmp(p, "CJK1", 4) != 0) {
        ESP_LOGW(TAG, "cjk12.bin bad magic, no chinese");
        return;
    }
    s_cjk.n = p[4] | (p[5] << 8);
    s_cjk.ascent = p[7];
    s_cjk.cps = p + 8;
    s_cjk.recs = s_cjk.cps + 2 * s_cjk.n;
    s_cjk.bmp = s_cjk.recs + 10 * s_cjk.n;
}

static int cjk_find(int cp)
{
    int lo = 0, hi = s_cjk.n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int v = s_cjk.cps[2 * mid] | (s_cjk.cps[2 * mid + 1] << 8);  // 逐字节读，不要求对齐
        if (v == cp) return mid;
        if (v < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

// 一个字形，不管来自哪套字库
typedef struct {
    const uint8_t *bmp;
    int w, h, xo, yo, adv;
} gl_t;

static void resolve(const font_t *f, int cp, gl_t *g)
{
    int slot = map_cp(cp);
    if (slot < 0 && s_cjk.n) {
        int i = cjk_find(cp);
        if (i >= 0) {
            const uint8_t *r = s_cjk.recs + 10 * i;
            uint32_t off = r[0] | (r[1] << 8) | (r[2] << 16) | ((uint32_t)r[3] << 24);
            g->bmp = s_cjk.bmp + off;
            g->w = r[4];
            g->h = r[5];
            g->xo = (int8_t)r[6];
            g->yo = (int8_t)r[7] + (f->ascent - s_cjk.ascent);  // 基线和主字体对齐
            g->adv = r[8];
            return;
        }
    }
    if (slot < f->first || slot >= f->first + f->count) {
        slot = '?';
    }
    const glyph_t *gl = &f->g[slot - f->first];
    g->bmp = f->bmp + gl->off;
    g->w = gl->w;
    g->h = gl->h;
    g->xo = gl->xo;
    g->yo = gl->yo;
    g->adv = gl->adv;
}

int text_width_sp(const font_t *f, const char *s, int sp)
{
    int w = 0, n = 0;
    gl_t g;
    while (*s) {
        resolve(f, next_char(&s), &g);
        w += g.adv;
        n++;
    }
    return w + (n > 1 ? (n - 1) * sp : 0);
}

int text_width(const font_t *f, const char *s)
{
    return text_width_sp(f, s, 0);
}

static void draw_glyphs(const font_t *f, int x, int y, const char *s, int sp, uint8_t r, uint8_t g,
                        uint8_t b, int a)
{
    gl_t gl;
    while (*s) {
        resolve(f, next_char(&s), &gl);
        int n = 0;
        for (int yy = 0; yy < gl.h; yy++) {
            for (int xx = 0; xx < gl.w; xx++, n++) {
                int v = (n & 1) ? (gl.bmp[n >> 1] & 0x0F) : (gl.bmp[n >> 1] >> 4);
                if (v) {
                    fb_blend(x + gl.xo + xx, y + gl.yo + yy, r, g, b, (v * a * 273) >> 12);  // v*a/15
                }
            }
        }
        x += gl.adv + sp;
    }
}

void text_draw(const font_t *f, int x, int y, const char *s, uint8_t r, uint8_t g, uint8_t b, int a)
{
    draw_glyphs(f, x + 1, y + 1, s, 0, 0, 0, 8, a * 2 / 3);
    draw_glyphs(f, x, y, s, 0, r, g, b, a);
}

void text_plain(const font_t *f, int x, int y, const char *s, int sp, uint8_t r, uint8_t g, uint8_t b, int a)
{
    draw_glyphs(f, x, y, s, sp, r, g, b, a);
}

int text_align(const font_t *f, int x, int y, int align, const char *s, int sp, uint8_t r, uint8_t g,
               uint8_t b, int a)
{
    int w = text_width_sp(f, s, sp);
    if (align == ALIGN_CENTER) {
        x -= w / 2;
    } else if (align == ALIGN_RIGHT) {
        x -= w;
    }
    draw_glyphs(f, x, y, s, sp, r, g, b, a);
    return w;
}

void text_fit(const font_t *f, char *s, size_t cap, int maxw)
{
    if (text_width(f, s) <= maxw) {
        return;
    }
    size_t n = strlen(s);
    while (n > 1) {
        s[--n] = 0;
        // 别把 UTF-8 截在半个字符上
        while (n > 1 && ((unsigned char)s[n - 1] & 0xC0) == 0x80) {
            s[--n] = 0;
        }
        if (n > 1 && ((unsigned char)s[n - 1] & 0xC0) == 0xC0) {
            s[--n] = 0;
        }
        if (n + 4 > cap) {
            continue;  // 放不下省略号，再砍一个字
        }
        memcpy(s + n, "\xE2\x80\xA6", 4);  // …
        if (text_width(f, s) <= maxw) {
            return;
        }
        s[n] = 0;
    }
}

// ---------------------------------------------------------------- 矩形、线、遮罩

void fb_fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, int a)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > GFX_W ? GFX_W : x + w, y1 = y + h > GFX_H ? GFX_H : y + h;
    if (x0 >= x1 || y0 >= y1 || a <= 0) {
        return;
    }
    if (a >= 255) {
        uint16_t p = pack(r, g, b);
        for (int yy = y0; yy < y1; yy++) {
            uint16_t *row = gfx_fb + yy * GFX_W;
            for (int xx = x0; xx < x1; xx++) {
                row[xx] = p;
            }
        }
        return;
    }
    int a5 = (a + 4) >> 3;
    uint16_t src = pack(r, g, b);
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = gfx_fb + yy * GFX_W;
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = blend565(row[xx], src, a5);
        }
    }
}

void fb_fill_add(int x, int y, int w, int h, int r, int g, int b)
{
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            fb_add(xx, yy, r, g, b);
        }
    }
}

void fb_frame(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, int a)
{
    fb_fill(x, y, w, 1, r, g, b, a);
    fb_fill(x, y + h - 1, w, 1, r, g, b, a);
    fb_fill(x, y + 1, 1, h - 2, r, g, b, a);
    fb_fill(x + w - 1, y + 1, 1, h - 2, r, g, b, a);
}

void fb_brackets(int x, int y, int w, int h, int len, uint8_t r, uint8_t g, uint8_t b, int a)
{
    fb_fill(x, y, len, 1, r, g, b, a);
    fb_fill(x, y, 1, len, r, g, b, a);
    fb_fill(x + w - len, y, len, 1, r, g, b, a);
    fb_fill(x + w - 1, y, 1, len, r, g, b, a);
    fb_fill(x, y + h - 1, len, 1, r, g, b, a);
    fb_fill(x, y + h - len, 1, len, r, g, b, a);
    fb_fill(x + w - len, y + h - 1, len, 1, r, g, b, a);
    fb_fill(x + w - 1, y + h - len, 1, len, r, g, b, a);
}

void fb_rbox(int x, int y, int w, int h, int rad, uint8_t r, uint8_t g, uint8_t b, int a)
{
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    if (rad <= 0) {
        fb_fill(x, y, w, h, r, g, b, a);
        return;
    }
    // 中间整块 + 上下两条，用快速填充；只有四个角逐像素算覆盖率
    fb_fill(x, y + rad, w, h - 2 * rad, r, g, b, a);
    fb_fill(x + rad, y, w - 2 * rad, rad, r, g, b, a);
    fb_fill(x + rad, y + h - rad, w - 2 * rad, rad, r, g, b, a);
    for (int cy = 0; cy < 2; cy++) {
        for (int cx = 0; cx < 2; cx++) {
            float ox = cx ? x + w - rad : x + rad;  // 圆心
            float oy = cy ? y + h - rad : y + rad;
            int bx = cx ? x + w - rad : x, by = cy ? y + h - rad : y;
            for (int yy = by; yy < by + rad; yy++) {
                for (int xx = bx; xx < bx + rad; xx++) {
                    float dx = xx + 0.5f - ox, dy = yy + 0.5f - oy;
                    float cov = rad + 0.5f - sqrtf(dx * dx + dy * dy);
                    if (cov <= 0.0f) continue;
                    fb_blend(xx, yy, r, g, b, cov >= 1.0f ? a : (int)(a * cov));
                }
            }
        }
    }
}

void fb_mask(int x0, int y0, int w, int h, const uint8_t *alpha, uint8_t r, uint8_t g, uint8_t b, float a)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int al = (int)(alpha[y * w + x] * a);
            if (al > 0) {
                fb_blend(x0 + x, y0 + y, r, g, b, al);
            }
        }
    }
}

void trail_clear(void)
{
    memset(s_trail, 0, TRAIL_W * TRAIL_H * 3);
    memset(s_row_lit, 0, sizeof(s_row_lit));
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
