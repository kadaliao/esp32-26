#include "shell.h"

#include "app.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gfx.h"
#include "input.h"
#include "link.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "shell";

// ============================================================ 登记表
// 新 app：实现一张 app_t，在 app.h 里 extern，再加到这里。顺序就是首页磁贴和左右划的顺序。
static const app_t *const APPS[] = {&app_sys, &app_magpie, &app_pond, &app_quota, &app_mirror, &app_eyes};
#define NAPPS ((int)(sizeof(APPS) / sizeof(APPS[0])))
#define PER_PAGE 4                                 // 首页一页 2×2
#define NPAGES ((NAPPS + PER_PAGE - 1) / PER_PAGE)

#define HOME        (-1)
#define TRANS_S     0.28f    // 切换动画时长
#define DOTS_S      1.6f     // 切换后页码点显示多久
#define STALE_MS    15000    // 这么久没收到某个 app 的数据，磁贴就变暗
#define SAVE_MS     3000     // 停在某个 app 这么久才写 NVS，免得来回划时反复写 flash
#define HEADER_H    30
#define TILE_GAP    8
#define N_STARS     60
#define MAX_RIPPLES 4
#define DIM_AFTER_MS 120000  // 主机这么久没消息（Mac 睡了 / 没开 deck）……
#define AWAKE_MS     60000   // ……并且这么久没人碰屏幕，就调暗
#define DIM_LEVEL    6       // 调暗到的亮度 %

typedef struct {
    float x, y, life;
    rgb_t col;
} ripple_t;

typedef struct {
    int16_t x, y;
    uint8_t base;
    float ph, sp;
} star_t;

static int s_cur = HOME;
static int s_page;               // 首页当前页
static uint16_t *s_prev;         // 切换前最后一帧（PSRAM），做滑动/淡入淡出
static float s_tt = 1.0f;        // 切换进度，1 = 没在切
static int s_tdir;               // +1 新画面从右边滑进来，-1 从左边，0 淡入淡出
static float s_dots;             // 页码点剩余显示时间
static float s_t;
static int64_t s_recv[NAPPS];    // 每个 app 最近一次收到数据
static bool s_bye;
static char s_clock[8] = "--:--";
static char s_date[16];
static char s_host[24];
static int64_t s_switch_ms;
static bool s_saved = true;
static int s_want_fps;           // 当前 app 想要的帧率，0 = 默认
static ripple_t s_rip[MAX_RIPPLES];
static uint8_t s_user_bright = 100;  // 主机设的亮度
static float s_bright = 100;         // 此刻的亮度（渐变）
static int s_bright_out = 100;       // 上一次真正写给背光的值
static int64_t s_input_ms;           // 最近一次有人碰屏幕 / 按键
static bool s_idle;
static star_t s_star[N_STARS];

// ============================================================ 小工具

static int find_app(const char *id)
{
    for (int i = 0; i < NAPPS; i++) {
        if (strcmp(APPS[i]->id, id) == 0) return i;
    }
    return HOME;
}

const char *shell_current(void)
{
    int c = s_cur;
    return c == HOME ? "home" : APPS[c]->id;
}

static void save_last(void)
{
    nvs_handle_t h;
    if (nvs_open("deck", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "last", shell_current());
        nvs_commit(h);
        nvs_close(h);
    }
    s_saved = true;
}

static void load_last(void)
{
    nvs_handle_t h;
    char id[16] = {0};
    size_t n = sizeof(id);
    if (nvs_open("deck", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "last", id, &n) == ESP_OK) {
            s_cur = find_app(id);
        }
        nvs_close(h);
    }
}

static void add_ripple(float x, float y, rgb_t col)
{
    int best = 0;
    for (int i = 0; i < MAX_RIPPLES; i++) {
        if (s_rip[i].life < s_rip[best].life) best = i;
    }
    s_rip[best] = (ripple_t){x, y, 1.0f, col};
}

// ============================================================ 切换

static void home_enter(void);

static void enter(int idx)
{
    trail_clear();
    if (idx == HOME) {
        home_enter();
    } else if (APPS[idx]->enter) {
        APPS[idx]->enter();
    }
}

static void switch_to(int idx, int dir)
{
    if (idx == s_cur) return;
    if (s_prev) {
        memcpy(s_prev, gfx_fb, GFX_W * GFX_H * 2);
        s_tt = 0;
        s_tdir = dir;
    }
    if (s_cur != HOME && APPS[s_cur]->leave) {
        APPS[s_cur]->leave();
    }
    s_want_fps = 0;  // 帧率要求是每个 app 自己的，进场时再要
    if (idx == HOME && s_cur != HOME) {
        s_page = s_cur / PER_PAGE;  // 回首页时停在刚才那个 app 所在的页
    }
    s_cur = idx;
    enter(idx);
    s_dots = idx == HOME ? 0 : DOTS_S;
    s_switch_ms = esp_timer_get_time() / 1000;
    s_saved = false;
    link_printf("!!APP %s\n", shell_current());
}

// 首页翻页：画面不变，只换磁贴，同样做滑动
static void flip_page(int d)
{
    if (s_prev) {
        memcpy(s_prev, gfx_fb, GFX_W * GFX_H * 2);
        s_tt = 0;
        s_tdir = d;
    }
    s_page = (s_page + d + NPAGES) % NPAGES;
}

// 首页 + 各 app 排成一个环：首页 → app0 → app1 → … → 首页
static void step_ring(int d)
{
    int pos = s_cur + 1;  // 首页是 0
    pos = (pos + d + NAPPS + 1) % (NAPPS + 1);
    switch_to(pos - 1, d > 0 ? 1 : -1);
}

void shell_go(const char *id)
{
    int idx = strcmp(id, "home") == 0 ? HOME : find_app(id);
    if (idx == HOME && strcmp(id, "home") != 0) return;  // 不认识的名字
    switch_to(idx, 0);
}

// 新画面已经画在 gfx_fb 里，把旧画面按进度叠上去
static void composite(float e)
{
    if (s_tdir == 0) {
        int k = (int)(e * 256);
        for (int i = 0; i < GFX_W * GFX_H; i++) {
            uint16_t a = s_prev[i], b = gfx_fb[i];
            if (a == b) continue;
            int r = ((a >> 11) * (256 - k) + (b >> 11) * k) >> 8;
            int g = (((a >> 5) & 63) * (256 - k) + ((b >> 5) & 63) * k) >> 8;
            int bl = ((a & 31) * (256 - k) + (b & 31) * k) >> 8;
            gfx_fb[i] = (uint16_t)((r << 11) | (g << 5) | bl);
        }
        return;
    }
    int o = (int)((1.0f - e) * GFX_W);  // 新画面还差多少像素没滑到位
    if (o <= 0) return;
    if (o > GFX_W) o = GFX_W;
    for (int y = 0; y < GFX_H; y++) {
        uint16_t *row = gfx_fb + y * GFX_W;
        const uint16_t *old = s_prev + y * GFX_W;
        if (s_tdir > 0) {  // 新的从右边进来，旧的往左走
            memmove(row + o, row, (size_t)(GFX_W - o) * 2);
            memcpy(row, old + (GFX_W - o), (size_t)o * 2);
        } else {           // 新的从左边进来，旧的往右走
            memmove(row, row + o, (size_t)(GFX_W - o) * 2);
            memcpy(row + (GFX_W - o), old, (size_t)o * 2);
        }
    }
}

// ============================================================ 数据

void shell_data(const char *id, const cJSON *root, int64_t now_ms)
{
    // 每帧都带时间；"deck" 是主机发给壳自己的（日期、主机名）
    char t[12];
    json_str(t, sizeof(t), root, "t");
    if (t[0]) {
        snprintf(s_clock, sizeof(s_clock), "%.5s", t);
    }
    s_bye = false;
    if (strcmp(id, "deck") == 0) {
        json_str(s_date, sizeof(s_date), root, "d");
        json_str(s_host, sizeof(s_host), root, "h");
        const cJSON *go = cJSON_GetObjectItemCaseSensitive(root, "go");
        if (cJSON_IsString(go)) shell_go(go->valuestring);
        return;
    }
    int i = find_app(id);
    if (i == HOME) {
        ESP_LOGW(TAG, "data for unknown app '%s'", id);
        return;
    }
    s_recv[i] = now_ms;
    APPS[i]->data(root, now_ms);
}

void shell_bye(void)
{
    s_bye = true;
    for (int i = 0; i < NAPPS; i++) {
        if (APPS[i]->bye) APPS[i]->bye();
    }
}

// ============================================================ 首页

static void tile_rect(int i, int *x, int *y, int *w, int *h)
{
    int col = i % 2, row = (i / 2) % 2;  // 页内位置：每页 4 个
    *w = (GFX_W - TILE_GAP * 3) / 2;
    *h = (GFX_H - HEADER_H - TILE_GAP * 2) / 2;
    *x = TILE_GAP + col * (*w + TILE_GAP);
    *y = HEADER_H + row * (*h + TILE_GAP);
}

static int hit_tile(int px, int py)
{
    for (int i = s_page * PER_PAGE; i < NAPPS && i < (s_page + 1) * PER_PAGE; i++) {
        int x, y, w, h;
        tile_rect(i, &x, &y, &w, &h);
        if (px >= x && px < x + w && py >= y && py < y + h) return i;
    }
    return HOME;
}

static void home_enter(void)
{
    // 深空：顶上近黑的蓝，往下透一点紫
    for (int y = 0; y < GFX_H; y++) {
        float t = (float)y / GFX_H;
        gfx_set_bg_row(y, (uint8_t)(5 + 8 * t), (uint8_t)(6 + 4 * t), (uint8_t)(16 + 16 * t));
    }
}

static void draw_tile(int i, int64_t now_ms, int px, int py, bool touching)
{
    const app_t *a = APPS[i];
    int x, y, w, h;
    tile_rect(i, &x, &y, &w, &h);
    bool fresh = a->camera || (s_recv[i] && now_ms - s_recv[i] < STALE_MS);
    int badge = a->badge && fresh ? a->badge(now_ms) : 0;
    bool pressed = touching && px >= x && px < x + w && py >= y && py < y + h;
    rgb_t c = a->color;

    fb_fill(x, y, w, h, 7, 9, 20, pressed ? 120 : 175);
    fb_frame(x, y, w, h, c.r, c.g, c.b, 28);
    float glow = badge == 2 ? 0.5f + 0.5f * sinf(s_t * 4.0f) : (badge == 1 ? 0.35f : 0.0f);
    fb_brackets(x, y, w, h, 7, c.r, c.g, c.b, (int)(110 + 120 * glow + (pressed ? 25 : 0)));

    text_plain(&font_tiny, x + 8, y + 6, a->name, 2, c.r, c.g, c.b, fresh ? 230 : 120);
    if (badge) {
        float b = badge == 2 ? 0.6f + 0.4f * sinf(s_t * 4.0f) : 0.7f;
        rgb_t bc = badge == 2 ? C_AMBER : c;
        fb_disc(x + w - 10, y + 10, 2.4f, bc.r, bc.g, bc.b, b);
    }

    tile_t t = {x + 8, y + 20, w - 16, h - 26, s_t};
    if (s_recv[i] == 0 && !a->camera) {
        text_align(&font_small, x + w / 2, y + h / 2 - 2, ALIGN_CENTER, "waiting for the mac", 0,
                   120, 120, 160, (int)(140 + 60 * sinf(s_t * 1.5f)));
    } else if (a->tile) {
        a->tile(&t, now_ms);
        if (!fresh) {
            fb_fill(x + 1, y + 18, w - 2, h - 19, 7, 9, 20, 150);  // 过期了：压暗
            text_align(&font_tiny, x + w / 2, y + h - 14, ALIGN_CENTER, "STALE", 2, 160, 140, 120, 200);
        }
    }
}

static void draw_home(float dt, int64_t now_ms)
{
    // 光层：几团各 app 主题色的光在磁贴后面慢慢游
    trail_fade(235);
    for (int i = 0; i < NAPPS; i++) {
        float ph = i * 1.7f;
        float mx = 160 + 140 * sinf(s_t * 0.05f * (1 + i * 0.3f) + ph);
        float my = 135 + 90 * sinf(s_t * 0.07f * (1 + i * 0.2f) + ph * 1.3f);
        rgb_t c = APPS[i]->color;
        trail_glow(mx, my, 36, c.r, c.g, c.b, 0.07f);
    }
    gfx_compose(GFX_H);
    for (int i = 0; i < N_STARS; i++) {
        const star_t *s = &s_star[i];
        int v = (int)(s->base * (0.55f + 0.45f * sinf(s_t * s->sp + s->ph)));
        fb_add(s->x, s->y, v * 3 / 4, v * 3 / 4, v);
    }

    // 顶栏：时间、日期、链路
    text_draw(&font_serif, 10, 3, s_clock, 232, 226, 255, 235);
    int cw = text_width(&font_serif, s_clock);
    if (s_date[0]) {
        text_plain(&font_tiny, 18 + cw, 12, s_date, 2, 140, 140, 190, 210);
    }
    int64_t rx = link_last_rx_ms();
    bool linked = rx && now_ms - rx < 5000 && !s_bye;
    const char *st = s_bye ? "MAC STOPPED" : (linked ? (s_host[0] ? s_host : "LINKED") : (rx ? "NO SIGNAL" : "NO HOST"));
    rgb_t sc = linked ? RGB(120, 220, 170) : RGB(220, 140, 120);
    int sw = text_align(&font_tiny, GFX_W - 10, 12, ALIGN_RIGHT, st, 1, sc.r, sc.g, sc.b, 220);
    fb_disc(GFX_W - 16 - sw, 16, 2.0f, sc.r, sc.g, sc.b, linked ? 0.6f + 0.4f * sinf(s_t * 2) : 0.8f);

    int px = 0, py = 0;
    bool touching = input_touching(&px, &py);
    for (int i = s_page * PER_PAGE; i < NAPPS && i < (s_page + 1) * PER_PAGE; i++) {
        draw_tile(i, now_ms, px, py, touching);
    }
    if (NPAGES > 1) {  // 页码点，放在底边的空隙里
        for (int p = 0; p < NPAGES; p++) {
            float a = p == s_page ? 0.9f : 0.3f;
            fb_disc(GFX_W / 2 + (p - (NPAGES - 1) / 2.0f) * 9, GFX_H - 4, p == s_page ? 2.0f : 1.5f, 200, 210, 255, a);
        }
    }
    (void)dt;
}

// ============================================================ 叠加层

static void draw_dots(void)
{
    float a = clampf(s_dots / 0.4f, 0, 1);
    int n = NAPPS + 1, gap = 10;
    int w = (n - 1) * gap;
    int x0 = (GFX_W - w) / 2, y = GFX_H - 9;
    fb_rbox(x0 - 9, y - 6, w + 18, 12, 6, 0, 0, 0, (int)(130 * a));
    for (int i = 0; i < n; i++) {
        bool cur = i == s_cur + 1;
        rgb_t c = i == 0 ? RGB(200, 200, 230) : APPS[i - 1]->color;
        fb_disc(x0 + i * gap, y, cur ? 2.6f : 1.6f, c.r, c.g, c.b, (cur ? 1.0f : 0.45f) * a);
    }
}

// 在别的 app 里：有 app 需要你看一眼时，顶上中间亮一个小点
static void draw_badges(int64_t now_ms)
{
    int k = 0;
    for (int i = 0; i < NAPPS; i++) {
        if (i == s_cur || !APPS[i]->badge) continue;
        if (!s_recv[i] || now_ms - s_recv[i] > STALE_MS) continue;
        if (APPS[i]->badge(now_ms) < 2) continue;
        rgb_t c = APPS[i]->color;
        float b = 0.55f + 0.45f * sinf(s_t * 4.0f + i);
        fb_disc(GFX_W / 2 - 6 + k * 8, 3, 2.0f, c.r, c.g, c.b, b);
        k++;
    }
}

static void draw_ripples(float dt)
{
    for (int i = 0; i < MAX_RIPPLES; i++) {
        ripple_t *r = &s_rip[i];
        if (r->life <= 0) continue;
        r->life -= dt * 2.2f;
        float k = clampf(r->life, 0, 1);
        fb_ring(r->x, r->y, 4 + 26 * (1 - k), 1.2f, r->col.r, r->col.g, r->col.b, 0.6f * k);
    }
}

// ============================================================ 背光

void shell_brightness(uint8_t percent)
{
    s_user_bright = percent;
}

bool shell_idle(void)
{
    return s_idle;
}

void shell_want_fps(int fps)
{
    s_want_fps = fps;
}

int shell_frame_ms(void)
{
    if (s_idle) return 100;  // 暗着：10fps 省电
    bool busy = s_tt < 1.0f || s_dots > 0;
    for (int i = 0; i < MAX_RIPPLES && !busy; i++) busy = s_rip[i].life > 0;
    return s_want_fps > 0 && !busy ? 1000 / s_want_fps : 33;
}

void shell_wake(int64_t now_ms)
{
    s_input_ms = now_ms;
}

static bool dimmed(void)
{
    return s_bright < s_user_bright * 0.6f;
}

static void step_backlight(float dt, int64_t now_ms)
{
    int x, y;
    if (input_touching(&x, &y)) s_input_ms = now_ms;
    int64_t rx = link_last_rx_ms();
    bool host_gone = s_bye || now_ms - rx > DIM_AFTER_MS;  // 从没收到过时 rx 是 0：开机两分钟后算
    bool idle = host_gone && now_ms - s_input_ms > AWAKE_MS;
    if (idle != s_idle) {
        ESP_LOGW(TAG, "backlight %s", idle ? "dim (no host, no touch)" : "wake");
        s_idle = idle;
    }
    float target = s_idle ? (s_user_bright < DIM_LEVEL ? s_user_bright : DIM_LEVEL) : s_user_bright;
    s_bright = approach(s_bright, target, s_idle ? 0.8f : 8.0f, dt);  // 慢慢暗下去，一碰就亮
    int out = (int)lrintf(s_bright);
    if (out != s_bright_out) {
        board_backlight((uint8_t)out);
        s_bright_out = out;
    }
}

// ============================================================ 手势

static const char *ev_name(ev_type_t t)
{
    static const char *N[] = {"none", "tap", "long", "left", "right", "up", "down", "btn", "btnlong"};
    return (unsigned)t < sizeof(N) / sizeof(N[0]) ? N[t] : "?";
}

static void handle_input(int64_t now_ms)
{
    input_ev_t ev;
    while (input_take(&ev)) {
        link_printf("!!IN %s %d %d\n", ev_name(ev.type), ev.x, ev.y);  // 主机 -v 时能看到，调触摸方向用
        bool was_dim = dimmed();
        s_input_ms = now_ms;
        if (was_dim) continue;  // 暗着的时候，第一下只负责叫醒
        if (ev.type == EV_TAP) {
            add_ripple(ev.x, ev.y, s_cur == HOME ? RGB(200, 210, 255) : APPS[s_cur]->color);
        }
        if (s_tt < 0.6f && ev.type != EV_BTN && ev.type != EV_BTN_LONG) continue;  // 动画刚开始，别连着切
        if (s_cur != HOME && APPS[s_cur]->input && APPS[s_cur]->input(&ev)) continue;

        switch (ev.type) {
        case EV_TAP:
            if (s_cur == HOME) {
                int i = hit_tile(ev.x, ev.y);
                if (i != HOME) switch_to(i, 0);
            } else {
                s_dots = DOTS_S;  // 点一下看看自己在第几页
            }
            break;
        case EV_SWIPE_LEFT:
        case EV_SWIPE_RIGHT:
            if (s_cur == HOME && NPAGES > 1) {  // 首页不止一页：左右划是翻页，进 app 靠点
                flip_page(ev.type == EV_SWIPE_LEFT ? 1 : -1);
            } else {
                step_ring(ev.type == EV_SWIPE_LEFT ? 1 : -1);
            }
            break;
        case EV_BTN:
            step_ring(+1);
            break;
        case EV_SWIPE_UP:
        case EV_SWIPE_DOWN:
        case EV_LONG:
        case EV_BTN_LONG:
            switch_to(HOME, 0);
            break;
        default:
            break;
        }
    }
}

// ============================================================ 入口

void shell_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    s_prev = heap_caps_malloc(GFX_W * GFX_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_prev) {
        ESP_LOGW(TAG, "no transition buffer, switching without animation");
    }
    for (int i = 0; i < N_STARS; i++) {
        s_star[i].x = (int16_t)(rnd() % GFX_W);
        s_star[i].y = (int16_t)(rnd() % GFX_H);
        s_star[i].base = (uint8_t)(20 + rnd() % 80);
        s_star[i].ph = frand(0, PI2);
        s_star[i].sp = frand(0.4f, 2.0f);
    }
    for (int i = 0; i < NAPPS; i++) {
        APPS[i]->init();
    }
    load_last();
    enter(s_cur);
    ESP_LOGW(TAG, "%d apps, start on %s", NAPPS, shell_current());
}

void shell_frame(float dt, int64_t now_ms)
{
    s_t += dt;
    handle_input(now_ms);
    step_backlight(dt, now_ms);

    if (s_cur == HOME) {
        draw_home(dt, now_ms);
    } else {
        APPS[s_cur]->frame(dt, now_ms);
        draw_badges(now_ms);
    }
    if (s_tt < 1.0f) {
        s_tt += dt / TRANS_S;
        if (s_tt < 1.0f) composite(ease_out(s_tt));
    }
    if (s_dots > 0) {
        s_dots -= dt;
        draw_dots();
    }
    draw_ripples(dt);

    if (!s_saved && now_ms - s_switch_ms > SAVE_MS) {
        if (s_cur == HOME || !APPS[s_cur]->camera) {
            save_last();
        }
        s_saved = true;
    }
}
