#include "scene.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gfx.h"

// ============================================================ 参数

#define WATER_Y     198          // 水面线
#define MAX_PARTS   220
#define MAX_RINGS   16
#define N_STARS     72
#define N_WILD      4            // 没有名字的野萤火虫，让空池子也有生气
#define STALE_MS    15000        // 超过这么久没收到主机数据就算断线

#define PI2 6.2831853f

typedef struct {
    uint8_t r, g, b;
} rgb_t;

// 每个 agent 一个固定的栖息点。按出现顺序占位，离开后空出来。
// 位置是手摆的：错开、留出名字的位置，前几个大致居中。
static const float SLOTS[MAX_AGENTS][2] = {
    {160, 94},  {66, 70},  {254, 118}, {72, 140},
    {254, 58},  {160, 152}, {22, 112},  {298, 166},
};

typedef struct {
    bool used;
    bool leaving;
    char id[12];
    char name[24];
    char tool[16];
    rgb_t col;
    uint8_t state;
    uint8_t kids;
    int32_t age;
    int slot;

    float x, y;          // 当前位置
    float lx, ly;        // 名字锚点（慢跟随，避免文字抖）
    float lt;            // 自己的时间轴（随状态变速）
    float ph[4];         // 漫游相位
    float energy;        // 平滑后的亮度/活跃度 0..1
    float vis;           // 出现/消失的淡入淡出 0..1
    float emit;          // 火星发射累加器
    float born;          // 出生后的秒数
} fly_t;

typedef struct {
    float x, y, vx, vy, life, max;
    rgb_t col;
    float bright;
} part_t;

typedef struct {
    float x, y, r, speed, life, max, width;
    rgb_t col;
} ring_t;

typedef struct {
    int16_t x, y;
    uint8_t base;
    float ph, sp;
} star_t;

typedef struct {
    float x, y, ph[4], sp;
} wild_t;

static fly_t s_fly[MAX_AGENTS];
static part_t s_part[MAX_PARTS];
static ring_t s_ring[MAX_RINGS];
static star_t s_star[N_STARS];
static wild_t s_wild[N_WILD];

static snapshot_t s_snap;
static bool s_have;
static float s_t;
static uint32_t s_rng = 0x9E3779B9u;

// ============================================================ 小工具

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static float frand(float lo, float hi)
{
    return lo + (hi - lo) * (float)(rnd() & 0xFFFFFF) / (float)0xFFFFFF;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static float approach(float cur, float target, float rate, float dt)
{
    return cur + (target - cur) * (1.0f - expf(-rate * dt));
}

static rgb_t kind_color(const char *kind)
{
    if (strcmp(kind, "claude") == 0) return (rgb_t){255, 136, 84};   // 珊瑚
    if (strcmp(kind, "codex") == 0) return (rgb_t){86, 214, 255};    // 冰青
    if (strcmp(kind, "gemini") == 0) return (rgb_t){120, 150, 255};  // 蓝
    return (rgb_t){196, 140, 255};                                    // 紫
}

static rgb_t mix(rgb_t a, rgb_t b, float t)
{
    return (rgb_t){(uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
                   (uint8_t)(a.b + (b.b - a.b) * t)};
}

static const rgb_t WHITE = {255, 255, 255};
static const rgb_t AMBER = {255, 196, 110};

static void add_ring(float x, float y, float speed, float life, float width, rgb_t col)
{
    for (int i = 0; i < MAX_RINGS; i++) {
        if (s_ring[i].life <= 0) {
            s_ring[i] = (ring_t){x, y, 2, speed, life, life, width, col};
            return;
        }
    }
}

static void add_part(float x, float y, float vx, float vy, float life, rgb_t col, float bright)
{
    for (int i = 0; i < MAX_PARTS; i++) {
        if (s_part[i].life <= 0) {
            s_part[i] = (part_t){x, y, vx, vy, life, life, col, bright};
            return;
        }
    }
}

static void burst(float x, float y, int n, rgb_t col, float speed)
{
    for (int i = 0; i < n; i++) {
        float a = frand(0, PI2), v = frand(0.3f, 1.0f) * speed;
        add_part(x, y, cosf(a) * v, sinf(a) * v - 8, frand(0.8f, 1.8f), mix(col, WHITE, 0.3f), 1.0f);
    }
}

// ============================================================ 初始化

void scene_init(void)
{
    // 天空：顶部近黑的靛蓝，往下渐亮，贴近水面有一抹紫
    for (int y = 0; y < GFX_H; y++) {
        float t = (float)y / WATER_Y;
        if (t > 1) t = 1;
        float glow = powf(t, 6.0f);
        gfx_set_bg_row(y, (uint8_t)(4 + 10 * t + 26 * glow), (uint8_t)(5 + 7 * t + 8 * glow),
                       (uint8_t)(14 + 22 * t + 30 * glow));
    }
    // 水：比天空更深更冷
    for (int y = WATER_Y; y < GFX_H; y++) {
        float d = (float)(y - WATER_Y) / (GFX_H - WATER_Y);
        gfx_set_water_row(y, (uint8_t)(6 - 4 * d), (uint8_t)(8 - 4 * d), (uint8_t)(22 - 10 * d));
    }

    for (int i = 0; i < N_STARS; i++) {
        s_star[i].x = (int16_t)(rnd() % GFX_W);
        // 星星偏向天顶
        float u = frand(0, 1);
        s_star[i].y = (int16_t)(u * u * (WATER_Y - 30));
        s_star[i].base = (uint8_t)(30 + rnd() % 120);
        s_star[i].ph = frand(0, PI2);
        s_star[i].sp = frand(0.4f, 2.2f);
    }
    for (int i = 0; i < N_WILD; i++) {
        s_wild[i].x = frand(20, 300);
        s_wild[i].y = frand(60, 180);
        for (int k = 0; k < 4; k++) s_wild[i].ph[k] = frand(0, PI2);
        s_wild[i].sp = frand(0.05f, 0.12f);
    }
}

// ============================================================ 数据进来

static fly_t *find_fly(const char *id)
{
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (s_fly[i].used && !s_fly[i].leaving && strcmp(s_fly[i].id, id) == 0) {
            return &s_fly[i];
        }
    }
    return NULL;
}

static int free_slot(void)
{
    bool taken[MAX_AGENTS] = {0};
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (s_fly[i].used && !s_fly[i].leaving) taken[s_fly[i].slot] = true;  // 正在散去的不占位
    }
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (!taken[i]) return i;
    }
    return -1;
}

void scene_update(const snapshot_t *snap)
{
    s_snap = *snap;
    s_have = true;

    // 离开的
    for (int i = 0; i < MAX_AGENTS; i++) {
        fly_t *f = &s_fly[i];
        if (!f->used || f->leaving) continue;
        bool still = false;
        for (int k = 0; k < snap->n; k++) {
            if (strcmp(snap->a[k].id, f->id) == 0) still = true;
        }
        if (!still) {
            f->leaving = true;
            burst(f->x, f->y, 36, f->col, 45);
        }
    }

    // 新来的 / 更新的
    for (int k = 0; k < snap->n; k++) {
        const agent_info_t *a = &snap->a[k];
        fly_t *f = find_fly(a->id);
        if (!f) {
            int slot = free_slot();
            if (slot < 0) continue;
            for (int i = 0; i < MAX_AGENTS; i++) {
                if (!s_fly[i].used) {
                    f = &s_fly[i];
                    break;
                }
            }
            if (!f) continue;
            memset(f, 0, sizeof(*f));
            f->used = true;
            f->slot = slot;
            snprintf(f->id, sizeof(f->id), "%s", a->id);
            f->x = f->lx = SLOTS[slot][0];
            f->y = f->ly = SLOTS[slot][1] + 30;  // 从下方升起
            for (int j = 0; j < 4; j++) f->ph[j] = frand(0, PI2);
            f->state = a->state;
            f->col = kind_color(a->kind);
            add_ring(SLOTS[slot][0], SLOTS[slot][1], 70, 1.4f, 2.0f, f->col);
            burst(SLOTS[slot][0], SLOTS[slot][1], 24, f->col, 35);
        }
        // 换了工具 → 一圈涟漪
        if (a->state == ST_TOOL && (f->state != ST_TOOL || strcmp(f->tool, a->tool) != 0)) {
            add_ring(f->x, f->y, 40, 0.9f, 1.2f, f->col);
        }
        // 刚转入「等你」→ 琥珀色涟漪
        if (a->state == ST_WAIT && f->state != ST_WAIT) {
            add_ring(f->x, f->y, 30, 1.2f, 1.5f, AMBER);
        }
        snprintf(f->name, sizeof(f->name), "%s", a->name);
        snprintf(f->tool, sizeof(f->tool), "%s", a->tool);
        f->col = kind_color(a->kind);
        f->state = a->state;
        f->kids = a->kids;
        f->age = a->age;
    }
}

// ============================================================ 每帧

static bool link_ok(int64_t now_ms)
{
    return s_have && now_ms - s_snap.recv_ms < STALE_MS;
}

static void step_fly(fly_t *f, float dt, bool online)
{
    f->born += dt;
    float speed, amp_x, amp_y, sink, target_e;
    switch (f->state) {
    case ST_TOOL:  speed = 1.0f;  amp_x = 22; amp_y = 13; sink = 0;  target_e = 1.0f;  break;
    case ST_THINK: speed = 0.7f;  amp_x = 18; amp_y = 11; sink = 0;  target_e = 0.85f; break;
    case ST_WAIT:  speed = 0.18f; amp_x = 5;  amp_y = 4;  sink = 0;  target_e = 0.6f;  break;
    default:       speed = 0.08f; amp_x = 6;  amp_y = 4;  sink = 6;  target_e = 0.22f; break;
    }
    if (!online) {
        target_e *= 0.35f;
        speed *= 0.3f;
    }
    f->energy = approach(f->energy, target_e, 1.5f, dt);
    f->lt += dt * (0.25f + 1.6f * speed);

    float hx = SLOTS[f->slot][0], hy = SLOTS[f->slot][1] + sink;
    float tx = hx + amp_x * (sinf(f->lt * 1.13f + f->ph[0]) + 0.45f * sinf(f->lt * 2.71f + f->ph[1]));
    float ty = hy + amp_y * (sinf(f->lt * 1.57f + f->ph[2]) + 0.4f * sinf(f->lt * 3.3f + f->ph[3]));
    f->x = approach(f->x, tx, 2.5f, dt);
    f->y = approach(f->y, ty, 2.5f, dt);
    f->lx = approach(f->lx, hx, 0.8f, dt);
    f->ly = approach(f->ly, f->y, 0.8f, dt);

    if (f->leaving) {
        f->vis -= dt * 1.4f;
        if (f->vis <= 0) f->used = false;
    } else {
        f->vis = clampf(f->vis + dt * 0.9f, 0, 1);
    }

    // 干活时往外溅火星
    float rate = f->state == ST_TOOL ? 14.0f : (f->state == ST_THINK ? 6.0f : 0.0f);
    if (!online) rate = 0;
    f->emit += rate * dt * f->vis;
    while (f->emit >= 1.0f) {
        f->emit -= 1.0f;
        add_part(f->x + frand(-2, 2), f->y + frand(-2, 2), frand(-14, 14), frand(-26, -6),
                 frand(0.7f, 1.5f), mix(f->col, WHITE, frand(0.1f, 0.5f)), frand(0.5f, 1.0f));
    }
}

static float breathe(float rate, float ph)
{
    return 0.5f + 0.5f * sinf(s_t * rate + ph);
}

static void draw_fly_light(const fly_t *f)
{
    float I = f->energy * f->vis;
    if (f->state == ST_WAIT) {
        I *= 0.55f + 0.45f * breathe(2.2f, f->ph[0]);
    }
    if (f->state == ST_TOOL || f->state == ST_THINK) {
        I *= 0.85f + 0.15f * sinf(s_t * 23.0f + f->ph[1]) * sinf(s_t * 7.0f + f->ph[2]);
    }
    trail_glow(f->x, f->y, 14 + 10 * I, f->col.r, f->col.g, f->col.b, 0.55f * I);
    trail_glow(f->x, f->y, 6 + 3 * I, f->col.r, f->col.g, f->col.b, 0.9f * I);

    // 子 agent：绕着转的小月亮
    for (int k = 0; k < f->kids; k++) {
        float a = s_t * 2.4f + k * PI2 / f->kids + f->ph[3];
        float mx = f->x + cosf(a) * 13, my = f->y + sinf(a) * 9;
        trail_dot(mx, my, f->col.r, f->col.g, f->col.b, 0.9f * f->vis);
    }
}

static void draw_fly_core(const fly_t *f)
{
    float I = f->energy * f->vis;
    rgb_t c = mix(f->col, WHITE, 0.35f);
    fb_disc(f->x, f->y, 1.6f + 1.4f * I, c.r, c.g, c.b, 0.8f * I + 0.1f * f->vis);
    fb_disc(f->x, f->y, 0.9f + 0.5f * I, 255, 255, 255, 0.85f * I);

    if (f->state == ST_WAIT && !f->leaving) {
        float b = breathe(2.2f, f->ph[0]);
        fb_ring(f->x, f->y, 9 + 3 * b, 1.0f, AMBER.r, AMBER.g, AMBER.b, (0.15f + 0.3f * b) * f->vis);
    }
    for (int k = 0; k < f->kids; k++) {
        float a = s_t * 2.4f + k * PI2 / f->kids + f->ph[3];
        fb_disc(f->x + cosf(a) * 13, f->y + sinf(a) * 9, 0.9f, c.r, c.g, c.b, 0.9f * f->vis);
    }
}

static void fit(const font_t *font, char *s, int maxw)
{
    if (text_width(font, s) <= maxw) return;
    size_t n = strlen(s);
    while (n > 1) {
        s[--n] = 0;
        if (n + 2 < 24) {
            s[n] = '.';
            s[n + 1] = '.';
            s[n + 2] = 0;
            if (text_width(font, s) <= maxw) return;
            s[n] = 0;
        }
    }
}

static void draw_fly_label(const fly_t *f)
{
    float a = f->vis * (f->state == ST_SLEEP ? 0.45f : 0.95f);
    if (a <= 0.02f) return;

    char name[24];
    snprintf(name, sizeof(name), "%s", f->name);
    fit(&font_label, name, 104);

    char sub[32];
    rgb_t sc = mix(f->col, WHITE, 0.45f);
    switch (f->state) {
    case ST_TOOL:
        snprintf(sub, sizeof(sub), "%s", f->tool[0] ? f->tool : "working");
        break;
    case ST_THINK:
        snprintf(sub, sizeof(sub), "thinking");
        sc = (rgb_t){170, 170, 205};
        break;
    case ST_WAIT:
        snprintf(sub, sizeof(sub), "your turn");
        sc = AMBER;
        break;
    default:
        if (f->age >= 3600) snprintf(sub, sizeof(sub), "asleep %ldh", (long)(f->age / 3600));
        else snprintf(sub, sizeof(sub), "asleep %ldm", (long)(f->age / 60));
        sc = (rgb_t){130, 130, 165};
        break;
    }
    if (f->kids > 0 && f->state != ST_SLEEP) {
        size_t n = strlen(sub);
        snprintf(sub + n, sizeof(sub) - n, " +%d", f->kids);
    }

    rgb_t nc = mix(f->col, WHITE, 0.62f);
    int nw = text_width(&font_label, name), sw = text_width(&font_small, sub);
    int y = (int)lrintf(f->ly) + 11;
    if (y > WATER_Y - 30) y = WATER_Y - 30;
    int nx = (int)lrintf(f->lx) - nw / 2, sx = (int)lrintf(f->lx) - sw / 2;
    nx = nx < 3 ? 3 : (nx > GFX_W - 3 - nw ? GFX_W - 3 - nw : nx);
    sx = sx < 3 ? 3 : (sx > GFX_W - 3 - sw ? GFX_W - 3 - sw : sx);
    text_draw(&font_label, nx, y, name, nc.r, nc.g, nc.b, (int)(255 * a));
    text_draw(&font_small, sx, y + 15, sub, sc.r, sc.g, sc.b, (int)(230 * a));
}

static void draw_stars(void)
{
    for (int i = 0; i < N_STARS; i++) {
        const star_t *s = &s_star[i];
        float tw = 0.55f + 0.45f * sinf(s_t * s->sp + s->ph);
        int v = (int)(s->base * tw);
        fb_add(s->x, s->y, v * 3 / 4, v * 3 / 4, v);
        if (s->base > 130) {  // 亮星带十字微芒
            int h = v / 4;
            fb_add(s->x - 1, s->y, h, h, h);
            fb_add(s->x + 1, s->y, h, h, h);
            fb_add(s->x, s->y - 1, h, h, h);
            fb_add(s->x, s->y + 1, h, h, h);
        }
    }
}

static void step_wild(float dt, float strength)
{
    for (int i = 0; i < N_WILD; i++) {
        wild_t *w = &s_wild[i];
        w->ph[0] += dt * w->sp * 1.3f;
        w->ph[1] += dt * w->sp * 1.9f;
        w->x += (cosf(w->ph[0]) * 9 + sinf(w->ph[1] * 2.1f) * 4) * dt;
        w->y += (sinf(w->ph[1]) * 5 + cosf(w->ph[0] * 1.7f) * 3) * dt;
        if (w->x < 10) w->x = 10;
        if (w->x > 310) w->x = 310;
        if (w->y < 50) w->y = 50;
        if (w->y > WATER_Y - 12) w->y = WATER_Y - 12;
        // 慢慢地明灭，像真的萤火虫
        float b = sinf(s_t * (0.6f + i * 0.17f) + w->ph[2]);
        b = b > 0 ? b * b : 0;
        float I = b * strength;
        if (I > 0.01f) {
            trail_glow(w->x, w->y, 9, 200, 230, 120, 0.35f * I);
            trail_dot(w->x, w->y, 220, 255, 150, 0.9f * I);
        }
    }
}

static void draw_center(const font_t *f, int y, const char *s, rgb_t c, int a)
{
    text_draw(f, (GFX_W - text_width(f, s)) / 2, y, s, c.r, c.g, c.b, a);
}

static void draw_header(int64_t now_ms, bool online)
{
    const char *clock = s_have && s_snap.clock[0] ? s_snap.clock : "--:--";
    text_draw(&font_serif, 12, 6, clock, 232, 226, 255, online ? 235 : 120);

    char info[48];
    int busy = 0, waiting = 0;
    for (int i = 0; i < MAX_AGENTS; i++) {
        const fly_t *f = &s_fly[i];
        if (!f->used || f->leaving) continue;
        if (f->state == ST_TOOL || f->state == ST_THINK) busy++;
        if (f->state == ST_WAIT) waiting++;
    }
    if (!online) {
        snprintf(info, sizeof(info), s_have ? "signal lost" : "listening");
    } else if (busy && waiting) {  // \x7f 是字库里的 "·"
        snprintf(info, sizeof(info), "%d at work  \x7f  %d waiting", busy, waiting);
    } else if (busy) {
        snprintf(info, sizeof(info), "%d at work", busy);
    } else if (waiting) {
        snprintf(info, sizeof(info), "%d waiting for you", waiting);
    } else {
        snprintf(info, sizeof(info), "all quiet");
    }
    int w = text_width(&font_small, info);
    rgb_t c = waiting && online ? AMBER : (rgb_t){150, 150, 195};
    text_draw(&font_small, GFX_W - 12 - w, 14, info, c.r, c.g, c.b, 220);
}

void scene_frame(float dt, int64_t now_ms)
{
    s_t += dt;
    bool online = link_ok(now_ms);

    int alive = 0;
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (s_fly[i].used) {
            step_fly(&s_fly[i], dt, online);
            if (!s_fly[i].leaving) alive++;
        }
    }

    // ---- 光层：衰减 + 新光 ----
    trail_fade(222);
    step_wild(dt, alive ? 0.35f : 1.0f);
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (s_fly[i].used) draw_fly_light(&s_fly[i]);
    }
    for (int i = 0; i < MAX_PARTS; i++) {
        part_t *p = &s_part[i];
        if (p->life <= 0) continue;
        p->life -= dt;
        p->vx *= 1.0f - 1.2f * dt;
        p->vy = p->vy * (1.0f - 1.2f * dt) - 6.0f * dt;  // 轻轻往上飘
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        float k = p->life / p->max;
        trail_dot(p->x, p->y, p->col.r, p->col.g, p->col.b, k * p->bright);
    }

    // ---- 合成 ----
    gfx_compose(WATER_Y);
    draw_stars();
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (s_fly[i].used) draw_fly_core(&s_fly[i]);
    }
    for (int i = 0; i < MAX_RINGS; i++) {
        ring_t *r = &s_ring[i];
        if (r->life <= 0) continue;
        r->life -= dt;
        float k = clampf(r->life / r->max, 0, 1);
        r->r += r->speed * dt * (0.4f + k);  // 先快后慢
        fb_ring(r->x, r->y, r->r, r->width * (0.5f + k), r->col.r, r->col.g, r->col.b, 0.7f * k * k);
    }
    // 水面线：一道很淡的光
    for (int x = 0; x < GFX_W; x++) {
        float e = 1.0f - fabsf(x - 160.0f) / 160.0f;
        int v = (int)(10 + 26 * e);
        fb_add(x, WATER_Y - 1, v / 2, v / 2, v);
    }
    gfx_reflect(WATER_Y, s_t);

    // ---- 文字（不进倒影）----
    draw_header(now_ms, online);
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (s_fly[i].used) draw_fly_label(&s_fly[i]);
    }
    if (!s_have) {
        float a = clampf(s_t / 2.0f, 0, 1);
        draw_center(&font_title, 82, "agent pond", (rgb_t){236, 230, 255}, (int)(230 * a));
        draw_center(&font_small, 124, "waiting for the mac", (rgb_t){140, 140, 185},
                    (int)(200 * a * (0.6f + 0.4f * breathe(1.5f, 0))));
    } else if (online && alive == 0) {
        draw_center(&font_serif, 96, "the pond is still", (rgb_t){210, 205, 240},
                    (int)(110 + 80 * breathe(0.8f, 0)));
    } else if (!online) {
        draw_center(&font_small, WATER_Y - 18, "no word from the mac", (rgb_t){150, 150, 190}, 170);
    }
}
