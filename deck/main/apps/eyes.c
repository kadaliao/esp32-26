// EYES：一张会看你的卡通脸。整块屏幕是脸：白眼球描墨线、大瞳孔带两个高光、眉毛、腮红、会变的嘴。
//
// 摄像头找到人脸，眼睛就跟着你转（弹簧模型：转过去会稍微过冲再回弹），盯着你时瞳孔放大、偶尔冲你眨一只眼；
// 没看到脸时看画面里在动的东西，什么都没有就自己找事干（东张西望、挑一边眉毛好奇、嘟嘴吹口哨）；
// 没人久了犯困（半闭眼、打哈欠），再久就睡着冒 z；你回来时 ^^ 笑眼咧嘴；点它一下是 >_< 加脸红。
// 摄像头和人脸检测只在这个应用在前台时开。

#include <stdlib.h>

#include "app.h"
#include "esp_timer.h"
#include "face.h"
#include "gfx.h"
#include "lens.h"
#include "shell.h"

// ---------------------------------------------------------------- 尺寸与配色

#define EYE_GAP   66     // 眼睛中心离屏幕中线多远
#define EYE_Y     104
#define EYE_W     78
#define EYE_H     92
#define EYE_R     36     // 圆角很大：差不多是个椭圆
#define IRIS_R    28
#define PUPIL_R   12.5f
#define INK       3.2f   // 描边粗细
#define INK_LID   4.6f   // 眼皮那道线粗一点，像睫毛
#define MOUTH_Y   184
#define BLUSH_DX  96
#define BLUSH_Y   152

static const rgb_t C_SKIN_TOP = {255, 238, 218};
static const rgb_t C_SKIN_BOT = {250, 214, 192};
static const rgb_t C_INK = {72, 42, 52};
static const rgb_t C_SCLERA = {255, 255, 255};
static const rgb_t C_IRIS_TOP = {52, 32, 56};
static const rgb_t C_IRIS_BOT = {184, 118, 70};  // 下半边透出琥珀色
static const rgb_t C_PUPIL = {28, 16, 28};
static const rgb_t C_BLUSH = {255, 132, 146};
static const rgb_t C_MOUTH = {150, 50, 66};
static const rgb_t C_TONGUE = {242, 118, 132};
static const rgb_t C_TEAL = {64, 240, 210};  // 只用在调试框

// ---------------------------------------------------------------- 行为参数

#define FRESH_MS  400    // 检测结果多新才算「现在看得到」
#define LOST_S    1.2f   // 这么久没看到脸：人走了
#define GREET_S   5.0f   // 离开超过这么久再回来，笑着打招呼
#define DROWSY_S  25.0f  // 没人这么久：犯困
#define SLEEP_S   45.0f  // 这么久：睡着
#define STIR_MOVING 300  // 画面里动的格子超过这么多，算有动静（会把它吵醒）
#define GLANCE_MOVING 60 // 超过这么多，没看到脸时就往动的地方瞟
#define MAX_Z     3

typedef enum { ST_LOOK, ST_IDLE, ST_DROWSY, ST_SLEEP } state_t;
static const char *const ST_NAME[] = {"look", "idle", "drowsy", "sleep"};

typedef enum { M_SMILE, M_GRIN, M_O, M_YAWN, M_FLAT } mouth_t;
typedef enum { ACT_NONE, ACT_CURIOUS, ACT_WHISTLE } act_t;

// 表情：全是连续量，每帧往目标趋近，所以表情之间是渐变过去的
typedef struct {
    float lid;        // 上眼皮：0 睁开 … 1 闭成一道 ‿
    float lower;      // 下眼皮：0 … 1 往上顶成 ⌒（笑眼）
    float squeeze;    // >_<
    float pupil;      // 瞳孔大小（倍数）
    float scale;      // 眼睛大小（倍数）
    float brow_y;     // 眉毛上下（负 = 抬眉）
    float brow_tilt;  // 眉毛斜度（正 = 内侧抬起，委屈；负 = 内侧压低，生气）
    float brow_one;   // 只抬左边那条眉（好奇）
    float blush;
} expr_t;

// 弹簧：视线、头的位置都用它，转过去会过冲一点再回来，比直接插值活泼
typedef struct {
    float x, v;
} spring_t;

typedef struct {
    float x, y, life;
} zee_t;

static state_t s_state = ST_IDLE;
static float s_t;
static expr_t s_e, s_et;
static spring_t s_gx, s_gy, s_hx, s_hy;  // 瞳孔视线（-1..1）、整张脸的偏移（像素）
static float s_tx, s_ty;                 // 视线目标
static float s_micro_x, s_micro_y, s_next_micro;
static mouth_t s_mouth = M_SMILE, s_mouth_prev = M_SMILE;
static float s_mouth_mix = 1.0f;         // 换嘴型时新旧交叉淡入
static float s_blink = -1, s_next_blink, s_next_look;
static float s_wink = -1, s_next_wink;   // 冲你眨一只眼
static float s_greet_t, s_poke_t, s_yawn_t, s_next_yawn;
static act_t s_act;
static float s_act_t, s_next_act;
static int64_t s_seen_ms, s_stir_ms, s_enter_ms;
static zee_t s_z[MAX_Z];
static float s_next_z;
static bool s_debug;
static face_t s_face;
static uint32_t s_seq;

// ---------------------------------------------------------------- 画图小工具

// S3 的浮点单元没有开方指令，sqrtf 是库函数调用，每像素用太贵。
// 用平方根倒数的位运算初值 + 两次牛顿迭代，相对误差约 5e-6，抗锯齿够了
static inline float fast_sqrt(float x)
{
    union {
        float f;
        uint32_t i;
    } u = {x};
    u.i = 0x5f3759dfu - (u.i >> 1);
    float y = u.f;
    y = y * (1.5f - 0.5f * x * y * y);
    y = y * (1.5f - 0.5f * x * y * y);
    return x * y;
}

static inline float sat01(float v)
{
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

static inline uint16_t pack565(int r, int g, int b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// 圆形的覆盖率：只在边缘那一像素宽的环上开方
static inline float disc_cov(float dd, float r)
{
    float in = r - 0.5f, out = r + 0.5f;
    if (dd <= in * in) return 1.0f;
    if (dd >= out * out) return 0.0f;
    return out - fast_sqrt(dd);
}

static inline void clip_box(int *x0, int *y0, int *x1, int *y1)
{
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > GFX_W - 1) *x1 = GFX_W - 1;
    if (*y1 > GFX_H - 1) *y1 = GFX_H - 1;
}

// 两端圆头的粗线段（眉毛、>_<、一字嘴、微笑的弧都用它拼）
static void stroke(float x0, float y0, float x1, float y1, float w, rgb_t c, float alpha)
{
    if (alpha <= 0.01f) return;
    float hw = w * 0.5f;
    int bx0 = (int)floorf(fminf(x0, x1) - hw - 1), bx1 = (int)ceilf(fmaxf(x0, x1) + hw + 1);
    int by0 = (int)floorf(fminf(y0, y1) - hw - 1), by1 = (int)ceilf(fmaxf(y0, y1) + hw + 1);
    clip_box(&bx0, &by0, &bx1, &by1);
    float dx = x1 - x0, dy = y1 - y0, ll = dx * dx + dy * dy;
    float inv = ll > 0 ? 1.0f / ll : 0;
    for (int y = by0; y <= by1; y++) {
        for (int x = bx0; x <= bx1; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = sat01((px * dx + py * dy) * inv);
            float ex = px - dx * t, ey = py - dy * t;
            float cov = disc_cov(ex * ex + ey * ey, hw);
            if (cov > 0) fb_blend(x, y, c.r, c.g, c.b, (int)(cov * alpha * 255));
        }
    }
}

// 抛物线弧：y = cy + bend * (1 - (x/hw)^2)，bend > 0 是 ‿，< 0 是 ⌒。拼成几段粗线
static void arc(float cx, float cy, float hw, float bend, float w, rgb_t c, float alpha)
{
    const int n = 6;
    float px = cx - hw, py = cy;
    for (int i = 1; i <= n; i++) {
        float u = -1.0f + 2.0f * i / n;
        float x = cx + u * hw, y = cy + bend * (1.0f - u * u);
        stroke(px, py, x, y, w, c, alpha);
        px = x, py = y;
    }
}

// 软软的椭圆光斑（腮红）
static void soft_oval(float cx, float cy, float rx, float ry, rgb_t c, float alpha)
{
    if (alpha <= 0.01f) return;
    int x0 = (int)(cx - rx), x1 = (int)(cx + rx), y0 = (int)(cy - ry), y1 = (int)(cy + ry);
    clip_box(&x0, &y0, &x1, &y1);
    float irx = 1.0f / rx, iry = 1.0f / ry;
    for (int y = y0; y <= y1; y++) {
        float qy = (y + 0.5f - cy) * iry;
        for (int x = x0; x <= x1; x++) {
            float qx = (x + 0.5f - cx) * irx;
            float q = 1.0f - (qx * qx + qy * qy);
            if (q <= 0) continue;
            fb_blend(x, y, c.r, c.g, c.b, (int)(q * q * alpha * 255));
        }
    }
}

// ---------------------------------------------------------------- 眼睛

typedef struct {
    float cx, cy, w, h, r;
    float lid, lower;     // 上下眼皮
    float ix, iy;         // 瞳孔中心相对眼睛中心的偏移（像素）
    float ir, pr;         // 虹膜、瞳孔半径
    float alpha;
} eye_t;

// 逐像素：眼白（圆角矩形距离场）和上下眼皮取交集得到「露出来的形状」，外面一圈墨线，
// 里面依次叠虹膜（上深下浅的渐变）、瞳孔、两个高光。眼皮压到底时形状没了，只剩墨线——就是闭眼的那道弧
static void draw_eye(const eye_t *e)
{
    float hw = e->w * 0.5f, hh = e->h * 0.5f;
    float r = fminf(e->r, fminf(hw, hh));
    float ink_max = INK_LID + 1.0f;
    int x0 = (int)floorf(e->cx - hw - ink_max), x1 = (int)ceilf(e->cx + hw + ink_max);
    int y0 = (int)floorf(e->cy - hh - ink_max), y1 = (int)ceilf(e->cy + hh + ink_max);
    clip_box(&x0, &y0, &x1, &y1);
    // 眼皮边是抛物线，两端弯向眼眶：合到底时贴着下眼眶剩一道 ‿，下眼皮顶到头时贴着上眼眶剩一道 ⌒。
    // 弯曲程度按四次方长：半闭（犯困）时眼皮还是平的
    float l4 = e->lid * e->lid * e->lid * e->lid, w4 = e->lower * e->lower * e->lower * e->lower;
    float lid_base = e->cy - hh - 2 + e->lid * (e->h + 4), lid_bend = 0.36f * e->h * l4;
    float low_base = e->cy + hh + 2 - e->lower * (e->h + 4), low_bend = 0.36f * e->h * w4;
    float icx = e->cx + e->ix, icy = e->cy + e->iy;
    float hl1x = icx - e->ir * 0.34f, hl1y = icy - e->ir * 0.36f, hl1r = e->ir * 0.30f;  // 大高光
    float hl2x = icx + e->ir * 0.33f, hl2y = icy + e->ir * 0.30f, hl2r = e->ir * 0.13f;  // 小高光
    float inv_ir2 = 1.0f / (2.0f * e->ir);
    float c_in = (r - 0.5f) * (r - 0.5f), c_out = (r + ink_max) * (r + ink_max);
    float inv_hw2 = 1.0f / (hw * hw);
    uint16_t white = pack565(C_SCLERA.r, C_SCLERA.g, C_SCLERA.b);
    // 笑眼、闭眼时剩下的那道月牙整个涂成墨色：一道实心的 ⌒ / ‿，比描边夹白边好认
    float inked = sat01((fmaxf(e->lid, e->lower) - 0.6f) / 0.3f);
    for (int y = y0; y <= y1; y++) {
        float py = y + 0.5f - e->cy;
        float ay = fabsf(py) - (hh - r);
        uint16_t *row = gfx_fb + y * GFX_W;
        // 这一行里整块在眼白里面的那段 [f0, f1]（眼皮碰不到、离边缘一像素以上）：不用算距离场，
        // 虹膜左右范围 [i0, i1] 以外直接写白
        int f0 = 1, f1 = 0, i0 = 0, i1 = -1;
        float yc = y + 0.5f;
        if (e->alpha >= 1.0f && lid_base < yc - 1.0f && low_base > yc + 1.0f) {
            float hin = -1;
            if (ay <= 0) hin = hw - 0.5f;
            else if (ay < r - 0.5f) hin = (hw - r) + fast_sqrt((r - 0.5f) * (r - 0.5f) - ay * ay);
            if (hin > 0) {
                f0 = (int)ceilf(e->cx - hin - 0.5f);
                f1 = (int)floorf(e->cx + hin - 0.5f);
            }
            float iy = yc - icy;
            if (fabsf(iy) < e->ir + 1.0f) {
                i0 = (int)floorf(icx - e->ir - 1.0f);
                i1 = (int)ceilf(icx + e->ir + 1.0f);
            }
        }
        // 虹膜渐变这一行的颜色
        float gt = sat01((y + 0.5f - (icy - e->ir)) * inv_ir2);
        gt = gt * gt;
        int ir_r = (int)(C_IRIS_TOP.r + (C_IRIS_BOT.r - C_IRIS_TOP.r) * gt);
        int ir_g = (int)(C_IRIS_TOP.g + (C_IRIS_BOT.g - C_IRIS_TOP.g) * gt);
        int ir_b = (int)(C_IRIS_TOP.b + (C_IRIS_BOT.b - C_IRIS_TOP.b) * gt);
        for (int x = x0; x <= x1; x++) {
            bool in = x >= f0 && x <= f1;
            if (in && (x < i0 || x > i1)) {
                row[x] = white;
                continue;
            }
            float a_ink = 1.0f, a_fill = 1.0f;
            if (!in) {
                float px = x + 0.5f - e->cx;
                float ax = fabsf(px) - (hw - r);
                float d;
                if (ax > 0 && ay > 0) {
                    float dd = ax * ax + ay * ay;
                    if (dd >= c_out) continue;
                    d = dd <= c_in ? -1.0f : fast_sqrt(dd) - r;
                } else {
                    d = (ax > ay ? ax : ay) - r;
                }
                float u2 = px * px * inv_hw2;
                float d_lid = (lid_base - lid_bend * u2) - yc;
                float d_low = yc - (low_base + low_bend * u2);
                float ink = INK;
                if (d_lid > d) d = d_lid, ink = INK_LID;
                if (d_low > d) d = d_low, ink = INK_LID;
                if (d >= ink + 0.5f) continue;
                a_ink = sat01(ink + 0.5f - d) * e->alpha;
                a_fill = sat01(0.5f - d) * e->alpha;
            }
            // 里面的颜色：眼白 → 虹膜 → 瞳孔 → 高光
            int cr = C_SCLERA.r, cg = C_SCLERA.g, cb = C_SCLERA.b;
            if (a_fill > 0) {
                float dx = x + 0.5f - icx, dy = y + 0.5f - icy, dd = dx * dx + dy * dy;
                float ai = disc_cov(dd, e->ir);
                if (ai > 0) {
                    // 虹膜外缘压一圈暗边
                    float rim = dd > (e->ir - 3.0f) * (e->ir - 3.0f) ? 0.55f : 1.0f;
                    int rr = (int)(ir_r * rim), rg = (int)(ir_g * rim), rb = (int)(ir_b * rim);
                    cr += (int)((rr - cr) * ai), cg += (int)((rg - cg) * ai), cb += (int)((rb - cb) * ai);
                    float ap = disc_cov(dd, e->pr);
                    if (ap > 0) {
                        cr += (int)((C_PUPIL.r - cr) * ap), cg += (int)((C_PUPIL.g - cg) * ap),
                            cb += (int)((C_PUPIL.b - cb) * ap);
                    }
                    float h1x = x + 0.5f - hl1x, h1y = y + 0.5f - hl1y;
                    float ah = disc_cov(h1x * h1x + h1y * h1y, hl1r);
                    float h2x = x + 0.5f - hl2x, h2y = y + 0.5f - hl2y;
                    float ah2 = disc_cov(h2x * h2x + h2y * h2y, hl2r);
                    if (ah2 > ah) ah = ah2;
                    if (ah > 0) cr += (int)((255 - cr) * ah), cg += (int)((255 - cg) * ah), cb += (int)((255 - cb) * ah);
                }
            }
            if (inked > 0) {
                cr += (int)((C_INK.r - cr) * inked), cg += (int)((C_INK.g - cg) * inked),
                    cb += (int)((C_INK.b - cb) * inked);
            }
            if (a_ink >= 1.0f && a_fill >= 1.0f) {
                row[x] = pack565(cr, cg, cb);
                continue;
            }
            if (a_ink > 0) fb_blend(x, y, C_INK.r, C_INK.g, C_INK.b, (int)(a_ink * 255));
            if (a_fill > 0) fb_blend(x, y, (uint8_t)cr, (uint8_t)cg, (uint8_t)cb, (int)(a_fill * 255));
        }
    }
}

// ---------------------------------------------------------------- 嘴

// 椭圆（可选只要下半截 = D 形的咧嘴笑）：墨线描边，里面嘴的颜色，下面一块舌头
static void mouth_oval(float cx, float cy, float rx, float ry, bool d_shape, float alpha)
{
    if (alpha <= 0.01f) return;
    int x0 = (int)(cx - rx - INK - 1), x1 = (int)(cx + rx + INK + 1);
    int y0 = (int)(cy - ry - INK - 1), y1 = (int)(cy + ry + INK + 1);
    clip_box(&x0, &y0, &x1, &y1);
    float m = fminf(rx, ry), irx = 1.0f / rx, iry = 1.0f / ry;
    float tcx = cx, tcy = cy + ry * 0.85f, tr = rx * 0.55f;  // 舌头
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float qx = (x + 0.5f - cx) * irx, qy = (y + 0.5f - cy) * iry;
            float d = (fast_sqrt(qx * qx + qy * qy) - 1.0f) * m;  // 近似的椭圆距离，够做抗锯齿
            if (d_shape) {
                float dt = cy - (y + 0.5f);  // 上边是平的
                if (dt > d) d = dt;
            }
            if (d >= INK + 0.5f) continue;
            float a_ink = sat01(INK + 0.5f - d) * alpha, a_fill = sat01(0.5f - d) * alpha;
            if (a_ink > 0) fb_blend(x, y, C_INK.r, C_INK.g, C_INK.b, (int)(a_ink * 255));
            if (a_fill > 0) {
                float tx = x + 0.5f - tcx, ty = y + 0.5f - tcy;
                float at = disc_cov(tx * tx + ty * ty, tr);
                rgb_t c = at > 0.5f ? C_TONGUE : C_MOUTH;
                fb_blend(x, y, c.r, c.g, c.b, (int)(a_fill * 255));
            }
        }
    }
}

static void draw_mouth_shape(mouth_t m, float cx, float cy, float alpha, float t)
{
    switch (m) {
    case M_SMILE:
        arc(cx, cy, 15, 6, 3.6f, C_INK, alpha);
        break;
    case M_GRIN:
        mouth_oval(cx, cy - 3, 19, 15, true, alpha);
        break;
    case M_O: {
        float k = 1.0f + 0.08f * sinf(t * 5.0f);  // 嘟着嘴微微动
        mouth_oval(cx, cy + 2, 6.5f * k, 8.0f * k, false, alpha);
        break;
    }
    case M_YAWN:
        mouth_oval(cx, cy + 4, 11, 16, false, alpha);
        break;
    case M_FLAT:
        stroke(cx - 8, cy + 3, cx + 8, cy + 3, 3.4f, C_INK, alpha);
        break;
    }
}

// ---------------------------------------------------------------- 行为

static void spring(spring_t *s, float target, float k, float c, float dt)
{
    for (int i = 0; i < 2; i++) {  // dt 可能到 0.1：拆两步积分，弹簧不会发散
        float h = dt * 0.5f;
        s->v += (k * (target - s->x) - c * s->v) * h;
        s->x += s->v * h;
    }
}

// 画面里在动的地方的重心（-1..1），动得太少返回 false
static bool motion_center(float *mx, float *my)
{
    lens_frame_t f;
    if (!lens_latest(&f) || f.moving < GLANCE_MOVING || f.moving > LENS_MW * LENS_MH / 3) return false;
    long sx = 0, sy = 0, n = 0;
    for (int y = 0; y < LENS_MH; y += 2) {
        const uint8_t *m = f.motion + y * LENS_MW;
        for (int x = 0; x < LENS_MW; x += 2) {
            if (m[x] > LENS_MOTION_T) {
                sx += x;
                sy += y;
                n++;
            }
        }
    }
    if (n < 8) return false;
    *mx = clampf(((float)sx / n - LENS_MW / 2) / (LENS_MW * 0.4f), -1, 1);
    *my = clampf(((float)sy / n - LENS_MH / 2) / (LENS_MH * 0.4f), -1, 1);
    return true;
}

static void bounce(float v)
{
    s_hy.v -= v;  // 往上蹦一下，弹簧拉回来
}

static void greet(void)
{
    s_greet_t = 1.6f;
    s_blink = 0;
    bounce(160);
}

static void poke(int64_t now_ms)
{
    s_poke_t = 0.8f;
    s_stir_ms = now_ms;
    bounce(220);
}

static void update(float dt, int64_t now_ms)
{
    lens_frame_t lf;
    if (lens_latest(&lf) && lf.moving > STIR_MOVING && lf.moving < LENS_MW * LENS_MH / 3) {
        s_stir_ms = now_ms;
    }

    bool see = face_latest(&s_face) && s_face.found && now_ms - s_face.at_ms < FRESH_MS;
    if (see && s_face.seq != s_seq) {
        s_seq = s_face.seq;
        float fx = s_face.x + s_face.w * 0.5f, fy = s_face.y + s_face.h * 0.5f;
        s_tx = clampf((fx - LENS_W / 2) / (LENS_W * 0.36f), -1, 1);
        s_ty = clampf((fy - LENS_H / 2) / (LENS_H * 0.36f), -1, 1);
        float away = (now_ms - (s_seen_ms ? s_seen_ms : s_enter_ms)) / 1000.0f;
        if (away > GREET_S || s_state >= ST_DROWSY) greet();
        s_seen_ms = now_ms;
        shell_wake(now_ms);  // 有人在看：别调暗
    }

    int64_t last = s_seen_ms > s_stir_ms ? s_seen_ms : s_stir_ms;
    if (last < s_enter_ms) last = s_enter_ms;
    float gone = (now_ms - last) / 1000.0f;
    float unseen = (now_ms - (s_seen_ms > s_enter_ms ? s_seen_ms : s_enter_ms)) / 1000.0f;
    state_t st = (see || unseen < LOST_S) ? ST_LOOK
               : gone < DROWSY_S          ? ST_IDLE
               : gone < SLEEP_S           ? ST_DROWSY
                                          : ST_SLEEP;
    if (st != s_state) {
        if (s_state == ST_SLEEP) s_blink = 0;  // 醒来先眨眼
        if (st == ST_DROWSY) s_next_yawn = s_t + 3.0f;
        s_state = st;
        s_act = ACT_NONE;
    }

    // 视线：看到脸就盯着（带一点微小的抖动，像真眼睛）；没看到就看动的地方，再没有就自己找方向
    if (s_state == ST_LOOK) {
        if (s_t > s_next_micro) {
            s_micro_x = frand(-0.06f, 0.06f), s_micro_y = frand(-0.05f, 0.05f);
            s_next_micro = s_t + frand(0.35f, 1.1f);
        }
    } else if (s_state != ST_SLEEP) {
        s_micro_x = s_micro_y = 0;
        float mx, my;
        if (motion_center(&mx, &my)) {
            s_tx = mx, s_ty = my;
            s_next_look = s_t + frand(1.0f, 2.5f);
        } else if (s_t > s_next_look) {
            bool center = (rnd() % 3) == 0;
            s_tx = center ? 0 : frand(-0.9f, 0.9f);
            s_ty = center ? 0 : frand(-0.6f, 0.5f);
            s_next_look = s_t + (s_state == ST_DROWSY ? frand(3, 6) : frand(1.0f, 2.8f));
        }
    }
    if (s_state == ST_SLEEP) s_tx = 0, s_ty = 0.3f;
    bool lazy = s_state == ST_DROWSY;
    spring(&s_gx, s_tx + s_micro_x, lazy ? 40 : 260, lazy ? 12 : 22, dt);
    spring(&s_gy, s_ty + s_micro_y, lazy ? 40 : 260, lazy ? 12 : 22, dt);
    // 脸跟着视线挪一点（像转头），加上呼吸的起伏
    spring(&s_hx, s_gx.x * 12.0f, 60, 10, dt);
    spring(&s_hy, s_gy.x * 7.0f + sinf(s_t * 1.6f) * 2.0f, 70, 9, dt);

    // 闲着没事干：随机来个小动作
    if (s_state == ST_IDLE && s_act == ACT_NONE && s_t > s_next_act) {
        s_act = (rnd() & 1) ? ACT_CURIOUS : ACT_WHISTLE;
        s_act_t = frand(1.4f, 2.6f);
        if (s_act == ACT_CURIOUS) s_tx = frand(-0.8f, 0.8f), s_ty = -0.4f;
        s_next_act = s_t + s_act_t + frand(3.0f, 8.0f);
    }
    if (s_act != ACT_NONE && (s_act_t -= dt) <= 0) s_act = ACT_NONE;
    // 犯困时隔一阵打个哈欠
    if (s_state == ST_DROWSY && s_t > s_next_yawn) {
        s_yawn_t = 2.2f;
        s_next_yawn = s_t + frand(7, 12);
    }
    if (s_yawn_t > 0) s_yawn_t -= dt;
    // 盯着你看一阵后，偶尔冲你眨一只眼
    if (s_state == ST_LOOK && s_wink < 0 && s_t > s_next_wink && s_greet_t <= 0) {
        s_wink = 0;
        s_next_wink = s_t + frand(18, 40);
    }
    if (s_wink >= 0 && (s_wink += dt / 0.6f) >= 1) s_wink = -1;
    if (s_greet_t > 0) s_greet_t -= dt;
    if (s_poke_t > 0) s_poke_t -= dt;

    // 表情目标
    expr_t t = {.pupil = 1.0f, .scale = 1.0f, .blush = 0.28f};
    mouth_t m = M_SMILE;
    bool close = see && s_face.w > LENS_W * 0.5f;  // 脸占了半个画面：凑太近了
    switch (s_state) {
    case ST_LOOK:
        t.pupil = 1.18f;  // 看到你：瞳孔放大
        t.blush = 0.38f;
        if (close) t.scale = 1.12f, t.pupil = 0.72f, t.brow_y = -8, m = M_O;
        break;
    case ST_IDLE:
        if (s_act == ACT_CURIOUS) t.brow_one = -9, t.brow_tilt = 0.1f, m = M_FLAT;
        if (s_act == ACT_WHISTLE) t.lower = 0.22f, m = M_O;
        break;
    case ST_DROWSY:
        t.lid = 0.5f, t.brow_y = 3, t.pupil = 0.95f, m = M_FLAT;
        if (s_yawn_t > 0) t.lid = 0.78f, t.brow_y = -5, t.brow_tilt = 0.18f, m = M_YAWN;
        break;
    case ST_SLEEP:
        t.lid = 1.0f, t.blush = 0.2f, m = M_O;
        break;
    }
    if (lens_state() == LENS_STARTING || lens_state() == LENS_OFF) t.lid = fmaxf(t.lid, 0.85f);  // 刚进来：先闭着眼
    if (s_greet_t > 0) t.lower = 1.0f, t.lid = 0, t.blush = 0.75f, t.brow_y = -7, m = M_GRIN;
    if (s_poke_t > 0) t.squeeze = 1.0f, t.blush = 0.9f, t.brow_tilt = -0.25f, t.brow_y = 2, m = M_O;
    s_et = t;

    float k = dt * 12.0f > 1 ? 1 : dt * 12.0f, kl = dt * 16.0f > 1 ? 1 : dt * 16.0f;
    s_e.lid += (s_et.lid - s_e.lid) * (s_et.lid < s_e.lid ? kl : k * 0.5f);  // 睁眼快、合眼慢
    s_e.lower += (s_et.lower - s_e.lower) * kl;
    s_e.squeeze += (s_et.squeeze - s_e.squeeze) * kl;
    s_e.pupil += (s_et.pupil - s_e.pupil) * k;
    s_e.scale += (s_et.scale - s_e.scale) * k;
    s_e.brow_y += (s_et.brow_y - s_e.brow_y) * k;
    s_e.brow_tilt += (s_et.brow_tilt - s_e.brow_tilt) * k;
    s_e.brow_one += (s_et.brow_one - s_e.brow_one) * k;
    s_e.blush += (s_et.blush - s_e.blush) * k * 0.6f;
    if (m != s_mouth) {
        s_mouth_prev = s_mouth;
        s_mouth = m;
        s_mouth_mix = 0;
    }
    if (s_mouth_mix < 1) s_mouth_mix = fminf(1.0f, s_mouth_mix + dt / 0.16f);

    // 眨眼：醒着时每隔几秒一次，偶尔连眨两下；犯困时又慢又少
    if (s_blink >= 0) {
        s_blink += dt / (s_state == ST_DROWSY ? 0.5f : 0.16f);
        if (s_blink >= 1) s_blink = -1;
    } else if (s_t > s_next_blink && s_state != ST_SLEEP) {
        s_blink = 0;
        bool twice = (rnd() % 6) == 0 && s_state != ST_DROWSY;
        s_next_blink = s_t + (twice ? 0.25f : (s_state == ST_DROWSY ? frand(3, 6) : frand(2.2f, 5.5f)));
    }

    // 睡着冒 z
    for (int i = 0; i < MAX_Z; i++) {
        if (s_z[i].life > 0) {
            s_z[i].life -= dt / 2.6f;
            s_z[i].y -= 22 * dt;
            s_z[i].x += 7 * dt;
        }
    }
    if (s_state == ST_SLEEP && s_e.lid > 0.9f && s_t > s_next_z) {
        for (int i = 0; i < MAX_Z; i++) {
            if (s_z[i].life <= 0) {
                s_z[i] = (zee_t){160 + EYE_GAP + 30, EYE_Y - 30, 1.0f};
                break;
            }
        }
        s_next_z = s_t + 0.95f;
    }
}

// ---------------------------------------------------------------- 整张脸

// 画一张脸：(cx, cy) 是两眼中间，k 是缩放（磁贴里用小的）
static void draw_face(float cx, float cy, float k, bool features)
{
    float blink = s_blink >= 0 ? 1.0f - fabsf(1.0f - 2.0f * s_blink) : 0.0f;  // 0 → 1 → 0
    float squeeze = s_e.squeeze;
    for (int i = 0; i < 2; i++) {
        float side = i ? 1.0f : -1.0f;
        float persp = 1.0f + 0.07f * s_gx.x * side;  // 看向哪边，那边的眼睛稍大
        float sc = s_e.scale * persp * k;
        eye_t e;
        e.w = EYE_W * sc * (1.0f + 0.06f * blink);  // 眨眼时略微变宽（挤压拉伸）
        e.h = EYE_H * sc;
        e.r = EYE_R * sc;
        e.cx = cx + side * EYE_GAP * k;
        e.cy = cy;
        float lid = fmaxf(s_e.lid, blink);
        float lower = s_e.lower;
        if (s_wink >= 0 && i == 1) lower = fmaxf(lower, 1.0f - fabsf(1.0f - 2.0f * s_wink));  // 右眼冲你眨一下
        e.lid = lid;
        e.lower = lower;
        e.ir = IRIS_R * sc;
        e.pr = PUPIL_R * sc * s_e.pupil;
        float maxx = e.w * 0.5f - e.ir * 0.72f, maxy = e.h * 0.5f - e.ir * 0.62f;
        e.ix = clampf(s_gx.x, -1.2f, 1.2f) * maxx;
        e.iy = clampf(s_gy.x, -1.2f, 1.2f) * maxy;
        e.alpha = 1.0f - squeeze;
        draw_eye(&e);
        if (squeeze > 0.01f) {  // >_<
            float ex = e.cx, ey = cy, s = 15 * k;
            float tip = -side * s * 0.75f;  // 左眼「>」尖朝里（右），右眼「<」尖朝里（左）
            stroke(ex - tip, ey - s, ex + tip, ey, 6.0f * k, C_INK, squeeze);
            stroke(ex + tip, ey, ex - tip, ey + s, 6.0f * k, C_INK, squeeze);
        }
        if (!features) continue;
        // 眉毛：内侧端点跟着 tilt 走；左边那条可以单独抬（好奇）
        float by = cy - EYE_H * 0.5f * s_e.scale * k - 16 * k + s_e.brow_y * k + (i == 0 ? s_e.brow_one * k : 0);
        float bx = e.cx + s_hx.x * 0.15f;
        float half = 17 * k, lift = s_e.brow_tilt * 40 * k;
        float inner_x = bx - side * half, outer_x = bx + side * half;
        stroke(inner_x, by - lift, outer_x, by + lift * 0.3f, 6.5f * k, C_INK, 1.0f);
    }
    if (!features) return;
    soft_oval(cx - BLUSH_DX * k, BLUSH_Y - EYE_Y + cy, 22 * k, 12 * k, C_BLUSH, s_e.blush);
    soft_oval(cx + BLUSH_DX * k, BLUSH_Y - EYE_Y + cy, 22 * k, 12 * k, C_BLUSH, s_e.blush);
    float my = cy + (MOUTH_Y - EYE_Y) * k;
    if (s_mouth_mix < 1) draw_mouth_shape(s_mouth_prev, cx, my, 1.0f - s_mouth_mix, s_t);
    draw_mouth_shape(s_mouth, cx, my, s_mouth_mix, s_t);
}

static void draw_debug(void)
{
    char s[56];
    if (s_face.found) {
        snprintf(s, sizeof(s), "%s · face %.0f%% %.0fpx · %.0fms", ST_NAME[s_state], s_face.score * 100,
                 s_face.w, s_face.ms);
    } else {
        snprintf(s, sizeof(s), "%s · no face · %.0fms", ST_NAME[s_state], s_face.ms);
    }
    text_plain(&font_tiny, 6, GFX_H - 12, s, 0, C_INK.r, C_INK.g, C_INK.b, 220);
    fb_frame(6, 6, 80, 60, C_INK.r, C_INK.g, C_INK.b, 120);  // 左上角：脸在画面里的位置
    if (s_face.found) {
        fb_frame(6 + (int)(s_face.x / 4), 6 + (int)(s_face.y / 4), (int)(s_face.w / 4) + 1, (int)(s_face.h / 4) + 1,
                 C_TEAL.r, C_TEAL.g, C_TEAL.b, 255);
    }
}

// ---------------------------------------------------------------- app 接口

static void eyes_init(void)
{
    s_next_blink = 2.0f;
    s_next_wink = 15.0f;
    s_next_act = 4.0f;
    s_e = (expr_t){.lid = 1.0f, .pupil = 1.0f, .scale = 1.0f};
}

// 主机可以开关调试信息、戳它、让它打招呼、让它马上睡：
// @@eyes{"debug":1}、{"poke":1}、{"greet":1}、{"nap":1}
static void eyes_data(const cJSON *root, int64_t now_ms)
{
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "debug");
    if (cJSON_IsNumber(d) || cJSON_IsBool(d)) s_debug = cJSON_IsTrue(d) || (cJSON_IsNumber(d) && d->valueint);
    if (json_num(root, "poke", 0)) poke(now_ms);
    if (json_num(root, "greet", 0)) greet();
    if (json_num(root, "nap", 0)) s_seen_ms = s_stir_ms = s_enter_ms = now_ms - (int64_t)(SLEEP_S * 1000) - 1000;
}

static void eyes_enter(void)
{
    for (int y = 0; y < GFX_H; y++) {
        rgb_t c = mix(C_SKIN_TOP, C_SKIN_BOT, (float)y / GFX_H);
        gfx_set_bg_row(y, c.r, c.g, c.b);
    }
    int64_t now = esp_timer_get_time() / 1000;
    s_enter_ms = now;
    s_seen_ms = s_stir_ms = 0;
    s_state = ST_IDLE;
    s_e.lid = 1.0f;  // 一进来先闭着眼，摄像头起来了再睁开
    s_seq = 0;
    s_greet_t = s_poke_t = s_yawn_t = 0;
    s_act = ACT_NONE;
    memset(s_z, 0, sizeof(s_z));
    face_enable(true);
    lens_start();
}

static void eyes_leave(void)
{
    face_enable(false);
    lens_stop();
}

static void eyes_frame(float dt, int64_t now_ms)
{
    s_t += dt;
    update(dt, now_ms);
    gfx_compose(GFX_H);  // 只有肤色渐变的底，不用光层
    draw_face(160 + s_hx.x, EYE_Y + s_hy.x, 1.0f, true);

    for (int i = 0; i < MAX_Z; i++) {
        if (s_z[i].life <= 0) continue;
        // 一边往上飘一边变大，最后淡掉
        float a = s_z[i].life > 0.85f ? (1.0f - s_z[i].life) / 0.15f : fminf(1.0f, s_z[i].life / 0.35f);
        const font_t *f = s_z[i].life > 0.66f ? &font_label : (s_z[i].life > 0.33f ? &font_num : &font_big);
        text_plain(f, (int)s_z[i].x, (int)s_z[i].y - f->ascent, "z", 0, C_INK.r, C_INK.g, C_INK.b, (int)(220 * a));
    }

    if (lens_state() == LENS_FAILED) {
        text_align(&font_small, 160, GFX_H - 22, ALIGN_CENTER, "can't see: no camera", 0, C_INK.r, C_INK.g, C_INK.b,
                   200);
    }
    if (s_debug) draw_debug();
}

// 磁贴：一张小脸自己东张西望、眨眼（摄像头这时是关的）。借用全屏的状态画，画完还原
static void eyes_tile(const tile_t *t, int64_t now_ms)
{
    (void)now_ms;
    float tt = t->t;
    expr_t saved = s_e;
    spring_t gx = s_gx, gy = s_gy;
    float blink = s_blink, wink = s_wink;
    s_e = (expr_t){.pupil = 1.1f, .scale = 1.0f};
    s_gx.x = sinf(tt * 0.7f) * 0.7f + sinf(tt * 1.9f) * 0.2f;
    s_gy.x = sinf(tt * 0.5f) * 0.3f;
    float ph = fmodf(tt, 4.3f);
    s_blink = ph < 0.16f ? ph / 0.16f : -1;
    s_wink = -1;
    draw_face(t->x + t->w * 0.5f + s_gx.x * 4, t->y + t->h * 0.5f, 0.36f, false);
    s_e = saved;
    s_gx = gx, s_gy = gy;
    s_blink = blink, s_wink = wink;
}

static bool eyes_input(const input_ev_t *ev)
{
    if (ev->type != EV_TAP) return false;
    poke(esp_timer_get_time() / 1000);
    return true;
}

const app_t app_eyes = {
    .id = "eyes",
    .name = "EYES",
    .color = {255, 168, 140},
    .camera = true,
    .init = eyes_init,
    .data = eyes_data,
    .enter = eyes_enter,
    .leave = eyes_leave,
    .frame = eyes_frame,
    .tile = eyes_tile,
    .input = eyes_input,
};
