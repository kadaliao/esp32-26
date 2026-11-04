// MIRROR：光绘镜子。摄像头看到哪里在动，就把光层里对应的那一格点亮，
// 光层本来就会每帧衰减，于是挥手会拖出一道彩色光尾。
// 摄像头只在这个应用在前台时上电（enter 开、leave 关），画面不出板子。点一下换效果。

#include <stdlib.h>

#include "app.h"
#include "esp_timer.h"
#include "gfx.h"
#include "lens.h"
#include "relay.h"
#include "shell.h"

typedef struct {
    const char *id;    // 主机切效果用：@@mirror{"fx":"neon"}
    const char *name;
    const char *hint;
    int bright;    // 摄像头画面亮度 0..32，0 = 不画画面，只有光
    uint8_t keep;  // 光层每帧保留多少 /256，越大光尾越长；0 = 不按帧衰减
    int gain;      // 运动 → 光的强度
} fx_t;

enum { FX_PAINT, FX_MATRIX, FX_GHIBLI, FX_GHOST, FX_NEON, FX_PLAIN, FX_N };

static const fx_t FX[FX_N] = {
    [FX_PAINT] = {"paint", "LIGHT PAINT", "wave your hand", 11, 247, 6},
    [FX_MATRIX] = {"matrix", "MATRIX", "follow the white rabbit", 0, 0, 0},
    [FX_GHIBLI] = {"ghibli", "GHIBLI", "painted on the Mac", 32, 0, 0},
    [FX_GHOST] = {"ghost", "GHOST", "hold still and you vanish", 0, 240, 9},
    [FX_NEON] = {"neon", "NEON", "outlines only", 0, 0, 0},
    [FX_PLAIN] = {"plain", "PLAIN", "just a mirror", 32, 0, 0},
};

#define LABEL_S      2.4f  // 进场 / 换效果后，效果名显示多久
#define EDGE_T       70    // 轮廓：Sobel 梯度超过这个才算边
#define NEON_KEEP    100   // 霓虹：每来一帧新画面，旧轮廓留多少 /256
#define WAKE_MOVING  120   // 动的格子超过这么多，算作有人在（推迟调暗）
#define ALL_MOVING   (LENS_MW * LENS_MH / 3)  // 超过三分之一在变：多半是自动曝光在调，不画

static int s_fx;
static int64_t s_ghibli_ms;  // 切到 GHIBLI 的时刻：刚切过去还没回音时先别报错
static int64_t s_touch_ms;   // 最近一次有人碰：GHIBLI 让 Mac 满载，没人管就自动暂停
static bool s_paused;
#define GHIBLI_IDLE_MS (5 * 60 * 1000)
static float s_t, s_label;
static uint32_t s_seq;
static rgb_t s_pal[256];  // 一圈色相，光尾的颜色从这里取

static rgb_t hsv(float h, float s, float v)
{
    float i = floorf(h * 6.0f), f = h * 6.0f - i;
    float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float r, g, b;
    switch ((int)i % 6) {
    case 0: r = v, g = t, b = p; break;
    case 1: r = q, g = v, b = p; break;
    case 2: r = p, g = v, b = t; break;
    case 3: r = p, g = q, b = v; break;
    case 4: r = t, g = p, b = v; break;
    default: r = v, g = p, b = q; break;
    }
    return RGB((uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255));
}

// 动的地方点亮。颜色随时间转，所以一道光尾从头到尾是一段彩虹
static void paint_motion(const lens_frame_t *f, int gain)
{
    if (f->moving > ALL_MOVING) return;
    int base = (int)(s_t * 50.0f);
    const uint8_t *m = f->motion;
    for (int cy = 0; cy < LENS_MH; cy++) {
        for (int cx = 0; cx < LENS_MW; cx++) {
            int d = m[cy * LENS_MW + cx] - LENS_MOTION_T;
            if (d <= 0) continue;
            int k = d * gain;
            if (k > 255) k = 255;
            rgb_t c = s_pal[(base + (cx + cy) / 3) & 255];
            trail_put(cx, cy, c.r * k >> 8, c.g * k >> 8, c.b * k >> 8);
        }
    }
}

// 轮廓发光：亮度图上做 Sobel，梯度大的格子点亮，颜色按行慢慢漂
static void paint_edges(const lens_frame_t *f)
{
    int base = (int)(s_t * 30.0f);
    const uint8_t *L = f->luma;
    for (int y = 1; y < LENS_MH - 1; y++) {
        const uint8_t *a = L + (y - 1) * LENS_MW, *b = a + LENS_MW, *c = b + LENS_MW;
        rgb_t col = s_pal[(base + y * 2) & 255];
        for (int x = 1; x < LENS_MW - 1; x++) {
            int gx = (a[x + 1] + 2 * b[x + 1] + c[x + 1]) - (a[x - 1] + 2 * b[x - 1] + c[x - 1]);
            int gy = (c[x - 1] + 2 * c[x] + c[x + 1]) - (a[x - 1] + 2 * a[x] + a[x + 1]);
            int d = abs(gx) + abs(gy) - EDGE_T;
            if (d <= 0) continue;
            int k = d * 3;
            if (k > 255) k = 255;
            trail_put(x, y, col.r * k >> 8, col.g * k >> 8, col.b * k >> 8);
        }
    }
}

// ---------------------------------------------------------------- MATRIX：字符雨
// 屏幕切成 8×10 的字符格，每列一道往下掉的雨（雨头发白，尾巴渐暗）。
// 摄像头画面亮的格子字也亮，于是人从字符雨里浮出来。不走光层，直接往 fb 贴字。
#define MX_CW   8
#define MX_CH   10
#define MX_COLS (GFX_W / MX_CW)
#define MX_ROWS (GFX_H / MX_CH)
#define MX_LV   32  // 亮度分几档

static const char MX_CHARS[] = "0123456789ABCDEFHKLMNPRTUVXZ:=*+-<>|";
#define MX_NG ((int)sizeof(MX_CHARS) - 1)
static uint8_t s_mx_atlas[MX_NG][MX_CH * MX_CW];  // 4 位 alpha，左右镜像过（电影里的字就是反的）
static uint16_t s_mx_lut[2][MX_LV][16];             // [是不是雨头][亮度][alpha] → 565
static uint8_t s_mx_glyph[MX_ROWS][MX_COLS];
static float s_mx_head[MX_COLS], s_mx_speed[MX_COLS], s_mx_len[MX_COLS];
static float s_mx_lo = 40, s_mx_hi = 200;            // 画面亮度自动拉伸的上下限（慢慢跟）

static void mx_drop(int c, bool first)
{
    s_mx_head[c] = first ? frand(-MX_ROWS, MX_ROWS) : -frand(0, MX_ROWS * 0.8f);
    s_mx_speed[c] = frand(7, 20);   // 行/秒
    s_mx_len[c] = frand(6, 18);     // 尾巴几行
}

static inline uint16_t pack565(int r, int g, int b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// 开机时借 gfx_fb 当草稿：用 SF Mono 把每个字白底黑字画出来，读回绿色通道当 alpha
static void mx_init(void)
{
    for (int i = 0; i < MX_NG; i++) {
        char ch[2] = {MX_CHARS[i], 0};
        fb_fill(0, 0, 40, 30, 0, 0, 0, 255);
        text_plain(&font_mono, 10, 4, ch, 0, 255, 255, 255, 255);
        int base = 4 + font_mono.ascent;
        int ix0 = 99, ix1 = -1;
        for (int y = base - 8; y < base + 2; y++) {
            for (int x = 6; x < 22; x++) {
                if ((gfx_fb[y * GFX_W + x] >> 5) & 63) {
                    if (x < ix0) ix0 = x;
                    if (x > ix1) ix1 = x;
                }
            }
        }
        if (ix1 < 0) continue;
        int w = ix1 - ix0 + 1, off = w < MX_CW ? (MX_CW - w) / 2 : 0;
        for (int y = 0; y < MX_CH; y++) {
            for (int x = ix0; x <= ix1; x++) {
                int dx = MX_CW - 1 - (x - ix0 + off);  // 镜像
                if ((unsigned)dx >= MX_CW) continue;
                int g6 = (gfx_fb[(base - 8 + y) * GFX_W + x] >> 5) & 63;
                s_mx_atlas[i][y * MX_CW + dx] = (uint8_t)(g6 >> 2);
            }
        }
    }
    fb_fill(0, 0, 40, 30, 0, 0, 0, 255);
    for (int h = 0; h < 2; h++) {
        for (int q = 0; q < MX_LV; q++) {
            float v = (float)q / (MX_LV - 1);
            float r = h ? 150 + 90 * v : 40 * v * v, g = h ? 255 : 30 + 225 * v, b = h ? 160 + 70 * v : 70 * v;
            if (h) {  // 雨头：发白，暗处也亮一点
                r *= 0.4f + 0.6f * v, g *= 0.4f + 0.6f * v, b *= 0.4f + 0.6f * v;
            }
            for (int a = 0; a < 16; a++) {
                s_mx_lut[h][q][a] = pack565((int)(r * a / 15), (int)(g * a / 15), (int)(b * a / 15));
            }
        }
    }
    for (int c = 0; c < MX_COLS; c++) {
        mx_drop(c, true);
        for (int r = 0; r < MX_ROWS; r++) s_mx_glyph[r][c] = (uint8_t)(rnd() % MX_NG);
    }
}

static void mx_step(float dt)
{
    for (int c = 0; c < MX_COLS; c++) {
        int before = (int)floorf(s_mx_head[c]);
        s_mx_head[c] += s_mx_speed[c] * dt;
        int now = (int)floorf(s_mx_head[c]);
        if (now != before && now >= 0 && now < MX_ROWS) {
            s_mx_glyph[now][c] = (uint8_t)(rnd() % MX_NG);  // 雨头每走一格换个新字
        }
        if (s_mx_head[c] - s_mx_len[c] > MX_ROWS) mx_drop(c, false);
    }
    for (int k = 0; k < 14; k++) {  // 零星的字自己变
        s_mx_glyph[rnd() % MX_ROWS][rnd() % MX_COLS] = (uint8_t)(rnd() % MX_NG);
    }
}

// 每格的画面亮度：亮度图（160×120）里 4×5 的一块取平均，再按 5%/95% 分位自动拉伸
static void mx_luma(const lens_frame_t *f, float dt, uint8_t cl[MX_ROWS][MX_COLS])
{
    const int bw = LENS_MW / MX_COLS, bh = LENS_MH / MX_ROWS;
    int hist[32] = {0};
    for (int r = 0; r < MX_ROWS; r++) {
        for (int c = 0; c < MX_COLS; c++) {
            int sum = 0;
            for (int y = 0; y < bh; y++) {
                const uint8_t *p = f->luma + (r * bh + y) * LENS_MW + c * bw;
                for (int x = 0; x < bw; x++) sum += p[x];
            }
            cl[r][c] = (uint8_t)(sum / (bw * bh));
            hist[cl[r][c] >> 3]++;
        }
    }
    int n = MX_ROWS * MX_COLS, acc = 0, lo = -1, hi = 31;
    for (int i = 0; i < 32; i++) {
        acc += hist[i];
        if (lo < 0 && acc > n / 20) lo = i;
        if (acc > n - n / 20) {
            hi = i;
            break;
        }
    }
    s_mx_lo = approach(s_mx_lo, lo * 8.0f, 3.0f, dt);
    s_mx_hi = approach(s_mx_hi, hi * 8.0f + 8, 3.0f, dt);
}

static void mx_draw(const lens_frame_t *f, float dt)
{
    static uint8_t cl[MX_ROWS][MX_COLS];
    if (f) mx_luma(f, dt, cl);
    float inv = 1.0f / fmaxf(s_mx_hi - s_mx_lo, 24.0f);
    memset(gfx_fb, 0, GFX_W * GFX_H * 2);
    for (int r = 0; r < MX_ROWS; r++) {
        for (int c = 0; c < MX_COLS; c++) {
            float L = f ? clampf((cl[r][c] - s_mx_lo) * inv, 0, 1) : 0.0f;  // 摄像头还没起来：只有雨
            L *= L;  // 压暗中间调：只有亮处的字突出来，背景留给雨
            float d = s_mx_head[c] - r, R = 0;
            bool head = false;
            if (d >= 0 && d < 1) {
                R = 1, head = true;
            } else if (d >= 1 && d < s_mx_len[c]) {
                R = 1 - d / s_mx_len[c];
                R *= R;
            }
            float I = 0.5f * L + R * (0.3f + 0.7f * L);
            int q = (int)(I * (MX_LV - 1) + 0.5f);
            if (q <= 0) continue;
            if (q >= MX_LV) q = MX_LV - 1;
            const uint16_t *lut = s_mx_lut[head][q];
            const uint8_t *g = s_mx_atlas[s_mx_glyph[r][c]];
            uint16_t *out = gfx_fb + r * MX_CH * GFX_W + c * MX_CW;
            for (int y = 0; y < MX_CH; y++, out += GFX_W, g += MX_CW) {
                for (int x = 0; x < MX_CW; x++) {
                    if (g[x]) out[x] = lut[g[x]];
                }
            }
        }
    }
}

// ----------------------------------------------------------------

static void draw_status(void)
{
    if (lens_state() == LENS_FAILED) {
        text_align(&font_label, 160, 98, ALIGN_CENTER, "NO CAMERA", 2, C_RED.r, C_RED.g, C_RED.b, 230);
        text_align(&font_small, 160, 122, ALIGN_CENTER, lens_error(), 0, 200, 180, 190, 200);
    } else {
        int a = (int)(150 + 80 * sinf(s_t * 3.0f));
        text_align(&font_small, 160, 112, ALIGN_CENTER, "waking the camera…", 0, 210, 190, 235, a);
    }
}

// 效果名、几个效果的圆点、一行提示：进场和换效果时显示一会儿再淡掉
static void draw_label(void)
{
    if (s_label <= 0) return;
    float al = clampf(s_label / 0.5f, 0, 1);
    int a = (int)(255 * al);
    const fx_t *fx = &FX[s_fx];
    rgb_t c = C_PINK;

    int w = text_width_sp(&font_tiny, fx->name, 2) + 26;
    fb_rbox(160 - w / 2, 8, w, 19, 9, 8, 6, 18, a * 170 / 255);
    text_align(&font_tiny, 160, 13, ALIGN_CENTER, fx->name, 2, c.r, c.g, c.b, a);
    for (int i = 0; i < FX_N; i++) {
        float x = 160 + (i - (FX_N - 1) * 0.5f) * 10.0f;
        bool on = i == s_fx;
        fb_disc(x, 35, on ? 2.2f : 1.5f, c.r, c.g, c.b, (on ? 0.9f : 0.35f) * al);
    }
    int hw = text_width(&font_small, fx->hint) + 20;
    fb_rbox(160 - hw / 2, GFX_H - 27, hw, 18, 9, 8, 6, 18, a * 150 / 255);
    text_align(&font_small, 160, GFX_H - 24, ALIGN_CENTER, fx->hint, 0, 225, 220, 240, a);

    char s[40];
    if (s_fx == FX_GHIBLI) {
        relay_stats_t st;
        relay_stats(&st);
        snprintf(s, sizeof(s), "%.0f fps · %.0f ms · enc %.0f dec %.0f", st.fps, st.latency_ms, st.enc_ms, st.dec_ms);
    } else {
        snprintf(s, sizeof(s), "cam %.0f fps", lens_fps());
    }
    text_align(&font_tiny, GFX_W - 8, GFX_H - 12, ALIGN_RIGHT, s, 0, 150, 140, 170, a * 180 / 255);
}

static void mirror_init(void)
{
    for (int i = 0; i < 256; i++) {
        s_pal[i] = hsv(i / 256.0f, 0.75f, 1.0f);
    }
    mx_init();
}

// GHIBLI 要把画面送去 Mac：只在这个效果下开转发
static void sync_relay(void)
{
    if (s_fx == FX_GHIBLI) {
        s_ghibli_ms = s_touch_ms = esp_timer_get_time() / 1000;
        s_paused = false;
        relay_start();
        shell_want_fps(15);  // Mac 一秒只画得出十来帧：屏幕不用刷 30 次，省下的时间给板子编 JPEG
    } else {
        relay_stop();
        shell_want_fps(0);
    }
}

static void set_fx(int i)
{
    s_fx = i;
    sync_relay();
    trail_clear();
    s_label = LABEL_S;
}

// 不靠主机数据；主机可以选效果：@@mirror{"fx":"neon"}
static void mirror_data(const cJSON *root, int64_t now_ms)
{
    (void)now_ms;
    char fx[12];
    json_str(fx, sizeof(fx), root, "fx");
    for (int i = 0; i < FX_N; i++) {
        if (strcmp(fx, FX[i].id) == 0 && i != s_fx) set_fx(i);
    }
}

static void mirror_enter(void)
{
    for (int y = 0; y < GFX_H; y++) {
        float t = (float)y / GFX_H;
        gfx_set_bg_row(y, (uint8_t)(4 + 6 * t), (uint8_t)(3 + 3 * t), (uint8_t)(10 + 12 * t));
    }
    s_label = LABEL_S;
    s_seq = 0;
    lens_start();
    sync_relay();
}

static void mirror_leave(void)
{
    relay_stop();
    lens_stop();
}

// GHIBLI：显示 Mac 画好的画面；Mac 没回音时显示压暗的原画面和提示。
// 它会让 Mac 的性能核满载：5 分钟没人碰、或者屏幕自动调暗了，就停下来（点一下继续）
static void draw_ghibli(const lens_frame_t *f, bool have, int64_t now_ms)
{
    bool pause = shell_idle() || now_ms - s_touch_ms > GHIBLI_IDLE_MS;
    if (pause != s_paused) {
        s_paused = pause;
        if (pause) {
            relay_stop();
        } else {
            s_ghibli_ms = now_ms;
            relay_start();
        }
    }
    if (s_paused) {
        if (have) gfx_compose_img(f->img, 10);
        else gfx_compose(GFX_H);
        text_align(&font_label, 160, 98, ALIGN_CENTER, "paused", 2, 235, 225, 245, 230);
        text_align(&font_small, 160, 120, ALIGN_CENTER, "tap to keep painting", 0, 190, 180, 210, 200);
        return;
    }
    const uint16_t *img;
    uint32_t seq;
    relay_stats_t st;
    relay_stats(&st);
    if (relay_latest(&img, &seq) && now_ms - st.last_ms < 2500) {
        gfx_compose_img(img, 32);
        return;
    }
    if (have) {
        gfx_compose_img(f->img, 10);
    } else {
        gfx_compose(GFX_H);
        draw_status();
        return;
    }
    bool slow = now_ms - s_ghibli_ms > 5000;  // 第一次要在 Mac 上装依赖、下模型，给它点时间
    int a = (int)(170 + 70 * sinf(s_t * 3.0f));
    text_align(&font_label, 160, 98, ALIGN_CENTER, slow ? "no reply from the Mac" : "painting on the Mac…", 0,
               235, 225, 245, a);
    text_align(&font_small, 160, 120, ALIGN_CENTER, slow ? "is deck.py running?" : "first time takes a while", 0,
               190, 180, 210, 200);
}

static void mirror_frame(float dt, int64_t now_ms)
{
    s_t += dt;
    if (s_label > 0) s_label -= dt;
    const fx_t *fx = &FX[s_fx];

    lens_frame_t f;
    bool have = lens_latest(&f);
    bool fresh = have && f.seq != s_seq;  // 摄像头比渲染慢，同一帧别加两次光
    if (fresh) {
        s_seq = f.seq;
        if (f.moving > WAKE_MOVING) shell_wake(now_ms);  // 在镜子前挥手也算有人
    }

    if (s_fx == FX_GHIBLI) {
        draw_ghibli(&f, have, now_ms);
        draw_label();
        return;
    }
    if (s_fx == FX_MATRIX) {
        mx_step(dt);
        mx_draw(have ? &f : NULL, dt);
        if (!have) draw_status();
        draw_label();
        return;
    }
    if (fx->keep) trail_fade(fx->keep);
    if (fresh) {
        if (s_fx == FX_NEON) {
            trail_fade(NEON_KEEP);  // 霓虹只在新帧到时换轮廓，没新帧就保持，免得一闪一闪
            paint_edges(&f);
        } else if (fx->gain) {
            paint_motion(&f, fx->gain);
        }
    }

    if (have && fx->bright > 0) {
        gfx_compose_img(f.img, fx->bright);
    } else {
        gfx_compose(GFX_H);
    }
    if (!have) draw_status();
    draw_label();
}

// 摄像头只在前台时开，磁贴里不放预览：画一个镜头，外圈绕着一颗光点
static void mirror_tile(const tile_t *t, int64_t now_ms)
{
    (void)now_ms;
    rgb_t c = C_PINK;
    float cx = t->x + 20, cy = t->y + t->h * 0.5f;
    fb_ring(cx, cy, 17, 2.0f, c.r, c.g, c.b, 0.85f);
    fb_ring(cx, cy, 10, 1.4f, c.r, c.g, c.b, 0.5f);
    fb_disc(cx - 3, cy - 3, 2.2f, 255, 255, 255, 0.6f);
    float a = t->t * 1.3f;
    fb_disc(cx + 13.5f * cosf(a), cy + 13.5f * sinf(a), 1.8f, 255, 230, 250, 0.85f);

    char name[24];
    snprintf(name, sizeof(name), "%s", FX[s_fx].name);
    text_fit(&font_label, name, sizeof(name), t->w - 46);
    text_plain(&font_label, t->x + 46, (int)cy - 17, name, 0, 235, 232, 245, 235);
    text_plain(&font_small, t->x + 46, (int)cy + 2, "wave at me", 0, c.r, c.g, c.b, 210);
}

static bool mirror_input(const input_ev_t *ev)
{
    if (ev->type != EV_TAP) return false;
    if (s_fx == FX_GHIBLI && s_paused) {  // 暂停着：这一下是「继续」，不换效果
        s_touch_ms = esp_timer_get_time() / 1000;
        return true;
    }
    set_fx((s_fx + 1) % FX_N);
    return true;
}

const app_t app_mirror = {
    .id = "mirror",
    .name = "MIRROR",
    .color = {255, 122, 217},
    .camera = true,
    .init = mirror_init,
    .data = mirror_data,
    .enter = mirror_enter,
    .leave = mirror_leave,
    .frame = mirror_frame,
    .tile = mirror_tile,
    .input = mirror_input,
};
