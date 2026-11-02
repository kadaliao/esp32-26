#include "scene.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "icons.h"

// ============================================================ 参数

#define HUB_X       160.0f
#define HUB_Y       136.0f
#define AGENT_X     44.0f
#define PROV_X      276.0f
#define NODE_TOP    66.0f
#define NODE_BOT    206.0f
#define MAX_NODES   5            // 每一侧最多几个
#define NODE_IDLE_MS 180000      // 3 分钟没请求就从边上撤掉
#define MAX_BIRDS   18
#define MAX_PARTS   200
#define MAX_RINGS   12
#define MAX_FLOATS  8
#define N_STARS     48
#define STALE_MS    5000         // 这么久没收到主机数据就算断线

#define PI  3.1415927f
#define PI2 6.2831853f

typedef struct {
    uint8_t r, g, b;
} rgb_t;

static const rgb_t WHITE = {255, 255, 255};
static const rgb_t GOLD = {255, 206, 120};
static const rgb_t RED = {255, 84, 84};

// ---- 两侧的节点 ----
typedef struct {
    bool used;
    char id[20];
    char name[20];
    char icon[9];
    rgb_t col;
    float y, ty;          // 当前 / 目标纵坐标
    float vis;            // 淡入淡出
    float heat;           // 平滑后的繁忙度
    float pulse;          // 有请求落地时闪一下
    int busy;             // 此刻在飞的请求数
    int64_t last_ms;      // 最近一次有请求
    bool leaving;
} node_t;

typedef enum { B_OUT = 0, B_HOLD, B_BACK } bird_state_t;

typedef struct {
    bool used;
    char id[16];
    char agent[20], prov[20];
    rgb_t col;
    uint8_t state;
    bool seen;            // 这一帧主机还说它在飞
    bool ok;
    bool done_pending;    // 落地消息到了，但还没飞到 provider
    int32_t out;
    float t;              // 当前航段进度 0..1
    float speed;          // 航段时长的倒数
    float off;            // 航线侧偏，避免叠在一起
    float x, y, px, py;   // 平滑后的位置 / 上一帧位置
    float hd;             // 朝向（弧度）
    float flap;           // 拍翅相位
    float orbit;          // 盘旋角
    float vis;
} bird_t;

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
    float x, y, life;
    char s[12];
    rgb_t col;
} floater_t;

typedef struct {
    int16_t x, y;
    uint8_t base;
    float ph, sp;
} star_t;

static node_t s_agents[MAX_NODES], s_provs[MAX_NODES];
static bird_t s_bird[MAX_BIRDS];
static part_t s_part[MAX_PARTS];
static ring_t s_ring[MAX_RINGS];
static floater_t s_float[MAX_FLOATS];
static star_t s_star[N_STARS];

static snapshot_t s_snap;
static bool s_have;
static float s_t;
static float s_hub_heat;
static int64_t s_now;
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

static rgb_t mix(rgb_t a, rgb_t b, float t)
{
    return (rgb_t){(uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
                   (uint8_t)(a.b + (b.b - a.b) * t)};
}

static uint32_t hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static const rgb_t PALETTE[] = {
    {255, 170, 92},  {120, 220, 160}, {150, 160, 255}, {255, 120, 170},
    {110, 210, 240}, {230, 200, 100}, {200, 140, 255}, {160, 230, 110},
};

static rgb_t agent_color(const char *id)
{
    if (strcmp(id, "claude") == 0) return (rgb_t){255, 136, 84};
    if (strcmp(id, "codex") == 0) return (rgb_t){86, 214, 255};
    if (strcmp(id, "gemini") == 0) return (rgb_t){120, 150, 255};
    return PALETTE[hash(id) % 8];
}

static rgb_t prov_color(const char *id)
{
    return PALETTE[(hash(id) >> 5) % 8];
}

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
        add_part(x, y, cosf(a) * v, sinf(a) * v, frand(0.5f, 1.3f), mix(col, WHITE, 0.3f), 1.0f);
    }
}

static void add_float(float x, float y, const char *s, rgb_t col)
{
    int best = 0;
    for (int i = 0; i < MAX_FLOATS; i++) {
        if (s_float[i].life <= 0) {
            best = i;
            break;
        }
        if (s_float[i].life < s_float[best].life) best = i;
    }
    for (int i = 0; i < MAX_FLOATS; i++) {
        if (i != best && s_float[i].life > 0 && fabsf(s_float[i].x - x) < 30 && fabsf(s_float[i].y - y) < 13) {
            y = s_float[i].y - 13;
        }
    }
    s_float[best] = (floater_t){x, y, 1.6f, "", col};
    snprintf(s_float[best].s, sizeof(s_float[best].s), "%s", s);
}

// ============================================================ 节点

static node_t *find_node(node_t *side, const char *id)
{
    for (int i = 0; i < MAX_NODES; i++) {
        if (side[i].used && strcmp(side[i].id, id) == 0) return &side[i];
    }
    return NULL;
}

// 找到或占一个位置；满了就挤掉最久没动静、此刻也没请求的那个
static node_t *touch_node(node_t *side, const char *id, const char *name, const char *icon, bool is_agent)
{
    if (!id[0]) return NULL;
    node_t *n = find_node(side, id);
    if (!n) {
        for (int i = 0; i < MAX_NODES && !n; i++) {
            if (!side[i].used) n = &side[i];
        }
        if (!n) {
            for (int i = 0; i < MAX_NODES; i++) {
                if (side[i].busy) continue;
                if (!n || side[i].last_ms < n->last_ms) n = &side[i];
            }
        }
        if (!n) return NULL;
        memset(n, 0, sizeof(*n));
        n->used = true;
        snprintf(n->id, sizeof(n->id), "%s", id);
        n->col = is_agent ? agent_color(id) : prov_color(id);
        n->y = n->ty = HUB_Y;
        add_ring(is_agent ? AGENT_X : PROV_X, HUB_Y, 50, 1.0f, 1.5f, n->col);
    }
    if (name[0]) snprintf(n->name, sizeof(n->name), "%s", name);
    if (icon[0]) snprintf(n->icon, sizeof(n->icon), "%s", icon);
    n->leaving = false;
    n->last_ms = s_now;
    return n;
}

static void layout_side(node_t *side)
{
    int k = 0, cnt = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (side[i].used && !side[i].leaving) cnt++;
    }
    for (int i = 0; i < MAX_NODES; i++) {
        if (!side[i].used || side[i].leaving) continue;
        side[i].ty = cnt == 1 ? HUB_Y : NODE_TOP + (NODE_BOT - NODE_TOP) * k / (cnt - 1);
        k++;
    }
}

static void node_pos(const char *id, bool is_agent, float *x, float *y)
{
    node_t *n = find_node(is_agent ? s_agents : s_provs, id);
    *x = is_agent ? AGENT_X : PROV_X;
    *y = n ? n->y : HUB_Y;
}

// ============================================================ 喜鹊

static bird_t *find_bird(const char *id)
{
    for (int i = 0; i < MAX_BIRDS; i++) {
        if (s_bird[i].used && strcmp(s_bird[i].id, id) == 0) return &s_bird[i];
    }
    return NULL;
}

static bird_t *spawn_bird(const req_t *r)
{
    bird_t *b = NULL;
    for (int i = 0; i < MAX_BIRDS && !b; i++) {
        if (!s_bird[i].used) b = &s_bird[i];
    }
    if (!b) return NULL;
    memset(b, 0, sizeof(*b));
    b->used = true;
    snprintf(b->id, sizeof(b->id), "%s", r->id);
    snprintf(b->agent, sizeof(b->agent), "%s", r->agent);
    snprintf(b->prov, sizeof(b->prov), "%s", r->prov);
    b->col = agent_color(r->agent);
    b->state = B_OUT;
    b->speed = frand(0.75f, 0.95f);
    b->off = frand(-14, 14);
    b->flap = frand(0, PI2);
    b->orbit = frand(0, PI2);
    node_pos(b->agent, true, &b->x, &b->y);
    b->px = b->x;
    b->py = b->y;
    return b;
}

static void land(bird_t *b, bool ok, int32_t out)
{
    b->ok = ok;
    b->out = out;
    if (b->state == B_OUT) {
        b->done_pending = true;  // 先飞到 provider 再折返
    } else if (b->state == B_HOLD) {
        b->state = B_BACK;
        b->t = 0;
        b->speed = 0.9f;
    }
}

void scene_update(const snapshot_t *snap)
{
    s_snap = *snap;
    s_have = true;

    // 主机退出了：两边的节点都撤掉，鸟在下面「主机不再提」那段里自动收回
    if (snap->bye) {
        for (int i = 0; i < MAX_NODES; i++) {
            s_agents[i].leaving = s_agents[i].used;
            s_provs[i].leaving = s_provs[i].used;
        }
    }

    for (int i = 0; i < MAX_BIRDS; i++) s_bird[i].seen = false;

    // 在飞的：确保每个都有一只鸟
    for (int k = 0; k < snap->nfly; k++) {
        const req_t *r = &snap->fly[k];
        touch_node(s_agents, r->agent, r->aname, r->aicon, true);
        touch_node(s_provs, r->prov, r->pname, r->picon, false);
        bird_t *b = find_bird(r->id);
        if (!b) b = spawn_bird(r);
        if (b) b->seen = true;
    }

    // 落地的：有鸟就让它折返，没鸟（直连、没经过网关）就补一只快速往返
    for (int k = 0; k < snap->ndone; k++) {
        const req_t *r = &snap->done[k];
        node_t *a = touch_node(s_agents, r->agent, r->aname, r->aicon, true);
        node_t *p = touch_node(s_provs, r->prov, r->pname, r->picon, false);
        if (p) p->pulse = 1;
        (void)a;
        bird_t *b = find_bird(r->id);
        if (!b) {
            b = spawn_bird(r);
            if (!b) continue;
            b->speed = 1.6f;  // 补的鸟飞快一点
        }
        b->seen = true;
        land(b, r->ok, r->out);
    }

    // 主机不再提、也没落地消息的：当作成功收尾
    for (int i = 0; i < MAX_BIRDS; i++) {
        bird_t *b = &s_bird[i];
        if (b->used && !b->seen && b->state != B_BACK && !b->done_pending) land(b, true, 0);
    }
}

// 航线：从 a 经过网关到 c 的二次贝塞尔（中点恰好穿过网关），外加一点侧偏
static void route(float ax, float ay, float cx, float cy, float t, float off, float *x, float *y)
{
    float bx = 2 * HUB_X - (ax + cx) / 2, by = 2 * HUB_Y - (ay + cy) / 2;
    float u = 1 - t;
    *x = u * u * ax + 2 * u * t * bx + t * t * cx;
    // 侧偏在两端和网关处都收拢为零，像从一个口子里钻过去；整体再往上拱一点
    *y = u * u * ay + 2 * u * t * by + t * t * cy + off * sinf(t * PI) * fabsf(1 - 2 * t) - 10.0f * sinf(t * PI);
}

static void step_bird(bird_t *b, float dt)
{
    float ax, ay, px, py, tx, ty;
    node_pos(b->agent, true, &ax, &ay);
    node_pos(b->prov, false, &px, &py);

    switch (b->state) {
    case B_OUT:
        b->t += dt * b->speed * 0.75f;
        if (b->t >= 1) {
            b->t = 0;
            if (b->done_pending) {
                b->state = B_BACK;
                b->speed = 1.0f;
            } else {
                b->state = B_HOLD;
                b->orbit = atan2f(b->y - py, b->x - (px - 14));
            }
        }
        route(ax, ay, px, py, b->t, b->off, &tx, &ty);
        break;
    case B_HOLD:
        b->orbit += dt * 2.6f;
        tx = px - 16 + cosf(b->orbit) * 10;
        ty = py + sinf(b->orbit) * 6;
        break;
    default:
        b->t += dt * b->speed * 0.8f;
        route(px, py, ax, ay, clampf(b->t, 0, 1), -b->off, &tx, &ty);
        if (b->t >= 1) {
            rgb_t c = b->ok ? GOLD : RED;
            burst(ax, ay, b->ok ? 14 : 22, c, b->ok ? 30 : 42);
            add_ring(ax, ay, 36, 0.7f, 1.2f, c);
            node_t *n = find_node(s_agents, b->agent);
            if (n) n->pulse = 1;
            if (!b->ok) {
                add_float(ax, ay - 14, "error", RED);
            } else if (b->out > 0) {
                char s[12];
                if (b->out >= 10000) snprintf(s, sizeof(s), "+%ldk", (long)(b->out / 1000));
                else snprintf(s, sizeof(s), "+%ld", (long)b->out);
                add_float(ax, ay - 14, s, GOLD);
            }
            b->used = false;
            return;
        }
        break;
    }

    b->px = b->x;
    b->py = b->y;
    b->x = approach(b->x, tx, 14, dt);
    b->y = approach(b->y, ty, 14, dt);
    float vx = b->x - b->px, vy = b->y - b->py;
    if (vx * vx + vy * vy > 0.0004f) {
        float want = atan2f(vy, vx);
        float d = want - b->hd;
        while (d > PI) d -= PI2;
        while (d < -PI) d += PI2;
        b->hd += d * (1 - expf(-10 * dt));
    }
    b->flap += dt * (b->state == B_HOLD ? 11.0f : 15.0f);
    b->vis = clampf(b->vis + dt * 3, 0, 1);

    // 拖在后面的光
    rgb_t c = b->state == B_BACK ? (b->ok ? GOLD : RED) : b->col;
    trail_glow(b->x, b->y, 7, c.r, c.g, c.b, 0.45f * b->vis);
    trail_dot(b->x, b->y, c.r, c.g, c.b, 0.9f * b->vis);
}

// 一只小喜鹊：黑头黑背、白肩白腹、长尾带一点蓝绿金属光，翅尖白
static void draw_bird(const bird_t *b)
{
    const float S = 1.6f;   // 整体尺寸
    const uint8_t K = 14;   // 近黑
    float cs = cosf(b->hd) * S, sn = sinf(b->hd) * S;  // 沿身体方向、已乘尺寸
    float nx = -sn, ny = cs;                           // 法线
    if (cs < 0) {  // 往左飞时翻过来，白肚皮保持在下方
        nx = -nx;
        ny = -ny;
    }
    float x = b->x, y = b->y, a = b->vis;

    // 长尾，带一点蓝绿金属光
    fb_bline(x - cs * 2, y - sn * 2, x - cs * 9 - nx * 0.5f, y - sn * 9 - ny * 0.5f, 1.8f * S, 20, 40, 64, a);
    fb_bline(x - cs * 6, y - sn * 6, x - cs * 10, y - sn * 10, 1.2f * S, 40, 90, 110, 0.7f * a);

    // 翅膀：翅尖在背上下方来回拍
    float f = sinf(b->flap);
    float wx = x - cs - nx * 5.5f * f, wy = y - sn - ny * 5.5f * f;
    fb_bline(x, y, wx, wy, 2.2f * S, K, K, K + 6, a);
    fb_bdisc(wx - cs * 0.6f, wy - sn * 0.6f, 1.0f * S, 235, 240, 250, a);

    // 身子、头、白腹、喙
    fb_bdisc(x, y, 2.4f * S, K, K, K + 6, a);
    fb_bdisc(x + cs * 3.2f, y + sn * 3.2f, 1.8f * S, K, K, K + 6, a);
    fb_bdisc(x - cs * 0.3f + nx * 1.2f, y - sn * 0.3f + ny * 1.2f, 1.3f * S, 238, 240, 248, a);
    fb_bdisc(x + cs * 5.0f, y + sn * 5.0f, 0.6f * S, 40, 40, 44, a);
}

// ============================================================ 初始化

void scene_init(void)
{
    // 黄昏的天：顶上深蓝，往下透一点暖紫，好让黑喜鹊有轮廓
    for (int y = 0; y < GFX_H; y++) {
        float t = (float)y / GFX_H;
        float glow = powf(t, 3.0f);
        gfx_set_bg_row(y, (uint8_t)(10 + 18 * t + 40 * glow), (uint8_t)(14 + 16 * t + 14 * glow),
                       (uint8_t)(34 + 26 * t + 22 * glow));
    }
    for (int i = 0; i < N_STARS; i++) {
        s_star[i].x = (int16_t)(rnd() % GFX_W);
        float u = frand(0, 1);
        s_star[i].y = (int16_t)(u * u * 150);
        s_star[i].base = (uint8_t)(20 + rnd() % 90);
        s_star[i].ph = frand(0, PI2);
        s_star[i].sp = frand(0.4f, 2.0f);
    }
}

// ============================================================ 画面

static void fit(const font_t *font, char *s, int maxw)
{
    if (text_width(font, s) <= maxw) return;
    size_t n = strlen(s);
    while (n > 1) {
        s[--n] = 0;
        if (n + 2 < 20) {
            s[n] = '.';
            s[n + 1] = '.';
            s[n + 2] = 0;
            if (text_width(font, s) <= maxw) return;
            s[n] = 0;
        }
    }
}

static void step_nodes(node_t *side, float dt)
{
    for (int i = 0; i < MAX_NODES; i++) side[i].busy = 0;
    for (int i = 0; i < MAX_BIRDS; i++) {
        if (!s_bird[i].used || s_bird[i].state == B_BACK) continue;
        node_t *n = find_node(side, side == s_agents ? s_bird[i].agent : s_bird[i].prov);
        if (n) {
            n->busy++;
            n->last_ms = s_now;
        }
    }
    for (int i = 0; i < MAX_NODES; i++) {
        node_t *n = &side[i];
        if (!n->used) continue;
        if (!n->leaving && !n->busy && s_now - n->last_ms > NODE_IDLE_MS) n->leaving = true;
    }
    layout_side(side);
    for (int i = 0; i < MAX_NODES; i++) {
        node_t *n = &side[i];
        if (!n->used) continue;
        n->y = approach(n->y, n->ty, 3, dt);
        n->heat = approach(n->heat, clampf(n->busy / 3.0f, 0, 1), 3, dt);
        n->pulse = approach(n->pulse, 0, 3, dt);
        if (n->leaving) {
            n->vis -= dt;
            if (n->vis <= 0) n->used = false;
        } else {
            n->vis = clampf(n->vis + dt * 1.5f, 0, 1);
        }
    }
}

static void node_light(const node_t *n, float x)
{
    float I = n->vis * (0.35f + 0.5f * n->heat + 0.5f * n->pulse);
    trail_glow(x, n->y, 10 + 8 * n->heat, n->col.r, n->col.g, n->col.b, 0.35f * I);
}

static void draw_node(const node_t *n, float x, bool is_agent)
{
    float a = n->vis;
    // 浅色圆徽章 + 图标；没图标时退回一个彩色光点
    fb_ring(x, n->y, 13 + 1.5f * sinf(s_t * 2 + x), 1.0f, n->col.r, n->col.g, n->col.b,
            (0.15f + 0.45f * n->heat + 0.4f * n->pulse) * a);
    const image_t *ic = icons_get(n->icon);
    if (ic) {
        fb_bdisc(x, n->y, 11.5f, 242, 243, 248, 0.95f * a);
        fb_image((int)lrintf(x) - ic->w / 2, (int)lrintf(n->y) - ic->h / 2, ic, a);
    } else {
        rgb_t c = mix(n->col, WHITE, 0.3f);
        fb_disc(x, n->y, 4.0f, c.r, c.g, c.b, 0.7f * a);
        fb_disc(x, n->y, 1.8f, 255, 255, 255, (0.4f + 0.5f * n->heat) * a);
    }

    char name[20];
    snprintf(name, sizeof(name), "%s", n->name[0] ? n->name : n->id);
    fit(&font_small, name, 82);
    int w = text_width(&font_small, name);
    int tx = (int)x - w / 2;
    tx = tx < 3 ? 3 : (tx > GFX_W - 3 - w ? GFX_W - 3 - w : tx);
    rgb_t nc = mix(n->col, WHITE, 0.6f);
    text_draw(&font_small, tx, (int)lrintf(n->y) + 14, name, nc.r, nc.g, nc.b, (int)(230 * a));
    (void)is_agent;
}

// 节点到网关的淡淡虚线
static void draw_lane(float x0, float y0, float heat, float vis, rgb_t col)
{
    float dx = HUB_X - x0, dy = HUB_Y - y0;
    int n = 22;
    float drift = fmodf(s_t * (0.3f + 1.2f * heat), 1.0f);
    for (int i = 1; i < n; i++) {
        float t = (i + drift) / n;
        float k = (0.06f + 0.22f * heat) * vis * sinf(t * PI);
        fb_disc(x0 + dx * t, y0 + dy * t, 0.6f, col.r, col.g, col.b, k);
    }
}

static void draw_hub(void)
{
    float h = s_hub_heat;
    float b = 0.5f + 0.5f * sinf(s_t * (1.2f + 2.5f * h));
    trail_glow(HUB_X, HUB_Y, 22 + 10 * h, 120, 170, 255, 0.25f + 0.35f * h);
    fb_ring(HUB_X, HUB_Y, 19 + 2 * b, 1.4f, 170, 200, 255, 0.35f + 0.3f * h);
    fb_ring(HUB_X, HUB_Y, 26 + 4 * b, 0.8f, 140, 170, 255, (0.1f + 0.2f * h) * (1 - b));
    fb_image((int)HUB_X - logo_m.w / 2, (int)HUB_Y - logo_m.h / 2, &logo_m, 1.0f);
    int w = text_width(&font_label, "magpie");
    text_draw(&font_label, (int)HUB_X - w / 2, (int)HUB_Y + 30, "magpie", 200, 212, 255, 200);
}

static void draw_center(const font_t *f, int y, const char *s, rgb_t c, int a)
{
    text_draw(f, (GFX_W - text_width(f, s)) / 2, y, s, c.r, c.g, c.b, a);
}

static int count_fly(void)
{
    int n = 0;
    for (int i = 0; i < MAX_BIRDS; i++) {
        if (s_bird[i].used && s_bird[i].state != B_BACK) n++;
    }
    return n;
}

static void draw_header(bool online)
{
    const char *clock = s_have && s_snap.clock[0] ? s_snap.clock : "--:--";
    if (s_have) fb_image(10, 9, &logo_s, online ? 1.0f : 0.5f);
    text_draw(&font_serif, s_have ? 36 : 12, 6, clock, 232, 226, 255, online ? 235 : 120);

    char info[40];
    int fly = count_fly();
    rgb_t c = {150, 160, 205};
    if (!online) {
        snprintf(info, sizeof(info), !s_have ? "listening" : (s_snap.bye ? "paused" : "signal lost"));
    } else if (s_snap.gw_down) {
        snprintf(info, sizeof(info), "gateway offline");
        c = RED;
    } else if (fly) {
        snprintf(info, sizeof(info), "%d in flight", fly);
        c = GOLD;
    } else {
        snprintf(info, sizeof(info), "all landed");
    }
    int w = text_width(&font_label, info);
    text_draw(&font_label, GFX_W - 12 - w, 6, info, c.r, c.g, c.b, 230);

    if (online && !s_snap.gw_down) {
        char today[40];  // \x7f 是字库里的 "·"
        snprintf(today, sizeof(today), "%ld today  \x7f  $%.2f", (long)s_snap.calls, s_snap.cost);
        w = text_width(&font_small, today);
        text_draw(&font_small, GFX_W - 12 - w, 23, today, 130, 140, 185, 210);
    }
}

void scene_frame(float dt, int64_t now_ms)
{
    s_t += dt;
    s_now = now_ms;
    bool online = s_have && !s_snap.bye && now_ms - s_snap.recv_ms < STALE_MS;

    step_nodes(s_agents, dt);
    step_nodes(s_provs, dt);
    s_hub_heat = approach(s_hub_heat, clampf(count_fly() / 5.0f, 0, 1), 2, dt);

    // ---- 光层 ----
    trail_fade(200);
    for (int i = 0; i < MAX_NODES; i++) {
        if (s_agents[i].used) node_light(&s_agents[i], AGENT_X);
        if (s_provs[i].used) node_light(&s_provs[i], PROV_X);
    }
    for (int i = 0; i < MAX_BIRDS; i++) {
        if (s_bird[i].used) step_bird(&s_bird[i], online || s_snap.bye ? dt : dt * 0.2f);
    }
    for (int i = 0; i < MAX_PARTS; i++) {
        part_t *p = &s_part[i];
        if (p->life <= 0) continue;
        p->life -= dt;
        p->vx *= 1.0f - 2.0f * dt;
        p->vy = p->vy * (1.0f - 2.0f * dt) + 10.0f * dt;  // 轻轻下落
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        trail_dot(p->x, p->y, p->col.r, p->col.g, p->col.b, p->life / p->max * p->bright);
    }

    // ---- 合成 ----
    gfx_compose(GFX_H);
    for (int i = 0; i < N_STARS; i++) {
        const star_t *s = &s_star[i];
        int v = (int)(s->base * (0.55f + 0.45f * sinf(s_t * s->sp + s->ph)));
        fb_add(s->x, s->y, v * 3 / 4, v * 3 / 4, v);
    }
    for (int i = 0; i < MAX_NODES; i++) {
        if (s_agents[i].used) draw_lane(AGENT_X, s_agents[i].y, s_agents[i].heat, s_agents[i].vis, s_agents[i].col);
        if (s_provs[i].used) draw_lane(PROV_X, s_provs[i].y, s_provs[i].heat, s_provs[i].vis, s_provs[i].col);
    }
    if (s_have && !s_snap.bye) draw_hub();
    for (int i = 0; i < MAX_NODES; i++) {
        if (s_agents[i].used) draw_node(&s_agents[i], AGENT_X, true);
        if (s_provs[i].used) draw_node(&s_provs[i], PROV_X, false);
    }
    for (int i = 0; i < MAX_RINGS; i++) {
        ring_t *r = &s_ring[i];
        if (r->life <= 0) continue;
        r->life -= dt;
        float k = clampf(r->life / r->max, 0, 1);
        r->r += r->speed * dt * (0.4f + k);
        fb_ring(r->x, r->y, r->r, r->width * (0.5f + k), r->col.r, r->col.g, r->col.b, 0.7f * k * k);
    }
    for (int i = 0; i < MAX_BIRDS; i++) {
        if (s_bird[i].used) draw_bird(&s_bird[i]);
    }
    for (int i = 0; i < MAX_FLOATS; i++) {
        floater_t *f = &s_float[i];
        if (f->life <= 0) continue;
        f->life -= dt;
        f->y -= 12 * dt;
        int w = text_width(&font_small, f->s);
        int x = (int)f->x - w / 2;
        x = x < 2 ? 2 : x;
        text_draw(&font_small, x, (int)f->y, f->s, f->col.r, f->col.g, f->col.b,
                  (int)(255 * clampf(f->life / 0.8f, 0, 1)));
    }

    // ---- 文字 ----
    draw_header(online);
    bool empty = true;
    for (int i = 0; i < MAX_NODES; i++) {
        if (s_agents[i].used || s_provs[i].used) empty = false;
    }
    if (!s_have) {
        float a = clampf(s_t / 2.0f, 0, 1);
        fb_image((GFX_W - logo_l.w) / 2, 40, &logo_l, a);
        draw_center(&font_title, 124, "magpie live", (rgb_t){236, 230, 255}, (int)(230 * a));
        draw_center(&font_small, 196, "waiting for the mac", (rgb_t){140, 140, 185},
                    (int)(200 * a * (0.7f + 0.3f * sinf(s_t * 1.5f))));
    } else if (s_snap.bye) {
        float a = clampf(1.0f - (empty ? 0.0f : 0.6f), 0, 1);
        fb_image((GFX_W - logo_l.w) / 2, 40, &logo_l, 0.6f * a);
        draw_center(&font_title, 124, "magpie live", (rgb_t){236, 230, 255}, (int)(170 * a));
        draw_center(&font_small, 196, "stopped on the mac", (rgb_t){140, 140, 185}, (int)(200 * a));
    } else if (!online) {
        draw_center(&font_small, 196, "no word from the mac", (rgb_t){150, 150, 190}, 170);
    } else if (s_snap.gw_down) {
        draw_center(&font_small, 196, "magpie is not running", (rgb_t){230, 140, 140}, 200);
    } else if (empty) {
        draw_center(&font_serif, 72, "quiet skies", (rgb_t){210, 205, 240},
                    (int)(110 + 70 * sinf(s_t * 0.8f)));
    }
}
