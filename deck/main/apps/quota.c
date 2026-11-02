// AI 用量：Claude Code / Codex / DeepSeek / Grok 的订阅额度、重置倒计时和余额
// （原 ai-subscription-display 项目，从 LVGL 改画到共用渲染器上，内容和配色规则不变）
#include "app.h"
#include "gfx.h"
#include "logos.h"

#define MAX_CARDS   6
#define MAX_WINDOWS 3

typedef struct {
    char key[8];       // "5h" / "7d"
    int used;          // 已用百分比 0..100
    int32_t reset_in;  // 距离重置的秒数，-1 表示未知
    char at[12];       // 重置的本地时刻，如 "at 22:59" / "Tue 09:00"，空 = 未知
} win_t;

typedef struct {
    char id[10];       // claude / codex / deepseek / grok
    char name[20];
    char plan[14];     // 套餐，如 Max / Pro
    char meta[28];     // 余额等附加信息
    int nwin;
    win_t win[MAX_WINDOWS];
} card_t;

typedef struct {
    bool valid;
    bool bye;
    int64_t recv_ms;
    char clock[9];
    int ncard;
    card_t card[MAX_CARDS];
} snapshot_t;

static snapshot_t s_snap;

// ---- 配色（沿用原面板）----
static const rgb_t C_CARD = {21, 26, 34};
static const rgb_t C_BORDER = {36, 44, 56};
static const rgb_t C_TEXT = {230, 237, 243};
static const rgb_t C_DIM = {139, 148, 158};
static const rgb_t C_TRACK = {46, 55, 66};
static const rgb_t C_OK = {63, 185, 80};
static const rgb_t C_WARN = {210, 153, 34};
static const rgb_t C_BAD = {248, 81, 73};

#define HEADER_H  22
#define PAD       5
#define GAP       4
#define CARD_PAD  4
#define TITLE_H   19
#define ROW_H     13
#define META_H    11

static const mask_t *logo_of(const char *id)
{
    if (strcmp(id, "claude") == 0) return &logo_claude;
    if (strcmp(id, "codex") == 0) return &logo_codex;
    if (strcmp(id, "deepseek") == 0) return &logo_deepseek;
    if (strcmp(id, "grok") == 0) return &logo_grok;
    return NULL;
}

static rgb_t accent_of(const char *id)
{
    if (strcmp(id, "claude") == 0) return RGB(0xD9, 0x77, 0x57);
    if (strcmp(id, "codex") == 0) return RGB(0x00, 0xA6, 0x7E);
    if (strcmp(id, "deepseek") == 0) return RGB(0x4D, 0x6B, 0xFE);
    if (strcmp(id, "grok") == 0) return RGB(0xC9, 0xD1, 0xD9);
    return C_DIM;
}

static rgb_t level_color(int left)
{
    return left >= 50 ? C_OK : (left >= 20 ? C_WARN : C_BAD);
}

static int critical_left(const card_t *c)
{
    int m = 100;
    for (int k = 0; k < c->nwin; k++) {
        int left = 100 - c->win[k].used;
        if (left < m) m = left;
    }
    return m;
}

static void fmt_eta(int32_t secs, char *buf, size_t cap)
{
    if (secs < 0) {
        snprintf(buf, cap, "--");
    } else if (secs == 0) {
        snprintf(buf, cap, "now");
    } else if (secs >= 86400) {
        snprintf(buf, cap, "%dd %dh", (int)(secs / 86400), (int)((secs % 86400) / 3600));
    } else if (secs >= 3600) {
        snprintf(buf, cap, "%dh%02dm", (int)(secs / 3600), (int)((secs % 3600) / 60));
    } else {
        snprintf(buf, cap, "%dm", (int)(secs / 60) > 0 ? (int)(secs / 60) : 1);
    }
}

// 用满了关心的是「几点能用」，倒计时反而要心算
static void win_eta(const win_t *w, int64_t age_s, char *buf, size_t cap)
{
    int32_t remain = w->reset_in;
    if (remain >= 0) {
        remain -= (int32_t)age_s;
        if (remain < 0) remain = 0;
    }
    if (w->used >= 100 && w->at[0] && remain > 0) {
        snprintf(buf, cap, "%s", w->at);
    } else {
        fmt_eta(remain, buf, cap);
    }
}

static void bar(int x, int y, int w, int h, int left, rgb_t c)
{
    fb_rbox(x, y, w, h, h / 2, C_TRACK.r, C_TRACK.g, C_TRACK.b, 255);
    int fw = w * (left < 0 ? 0 : left) / 100;
    if (fw >= h) {
        fb_rbox(x, y, fw, h, h / 2, c.r, c.g, c.b, 255);
    } else if (fw > 0) {
        fb_fill(x, y + 1, fw, h - 2, c.r, c.g, c.b, 255);
    }
}

// ============================================================ 画面

static int natural_h(const card_t *c)
{
    return CARD_PAD * 2 + TITLE_H + c->nwin * ROW_H + (c->meta[0] ? META_H : 0);
}

static void draw_card(const card_t *c, int x, int y, int w, int h, int64_t age_s)
{
    fb_rbox(x, y, w, h, 6, C_BORDER.r, C_BORDER.g, C_BORDER.b, 255);
    fb_rbox(x + 1, y + 1, w - 2, h - 2, 5, C_CARD.r, C_CARD.g, C_CARD.b, 255);

    rgb_t accent = accent_of(c->id);
    const mask_t *logo = logo_of(c->id);
    int ty = y + CARD_PAD;
    if (logo) fb_mask(x + 8, ty + (TITLE_H - logo->h) / 2, logo->w, logo->h, logo->a, accent.r, accent.g, accent.b, 1.0f);
    int nw = text_align(&font_label, x + 29, ty + 1, ALIGN_LEFT, c->name, 0, C_TEXT.r, C_TEXT.g, C_TEXT.b, 255);
    if (c->plan[0]) {
        text_plain(&font_small, x + 35 + nw, ty + 3, c->plan, 0, C_DIM.r, C_DIM.g, C_DIM.b, 255);
    }

    // 右上角大字：最紧的那个窗口还剩多少；没窗口就显示余额
    char val[28];
    rgb_t vc = C_TEXT;
    if (c->nwin > 0) {
        int left = critical_left(c);
        snprintf(val, sizeof(val), "%d%%", left);
        vc = level_color(left);
    } else if (c->meta[0]) {
        snprintf(val, sizeof(val), "%s", c->meta);
    } else {
        snprintf(val, sizeof(val), "--");
        vc = C_DIM;
    }
    text_align(&font_num, x + w - 10, ty - 2, ALIGN_RIGHT, val, 0, vc.r, vc.g, vc.b, 255);

    for (int k = 0; k < c->nwin; k++) {
        const win_t *wn = &c->win[k];
        int ry = ty + TITLE_H + k * ROW_H;
        int left = 100 - wn->used;
        text_plain(&font_tiny, x + 10, ry + 1, wn->key, 0, C_DIM.r, C_DIM.g, C_DIM.b, 255);
        bar(x + 30, ry + 3, w - 30 - 10 - 62, 6, left, level_color(left));
        char eta[16];
        win_eta(wn, age_s, eta, sizeof(eta));
        text_align(&font_tiny, x + w - 10, ry + 1, ALIGN_RIGHT, eta, 0, C_DIM.r, C_DIM.g, C_DIM.b, 255);
    }
    if (c->meta[0]) {
        text_plain(&font_tiny, x + 10, ty + TITLE_H + c->nwin * ROW_H, c->nwin == 0 ? "balance" : c->meta, 0,
                   C_DIM.r, C_DIM.g, C_DIM.b, 255);
    }
}

// 放不下完整卡片时（比如 magpie 配了五六家），每家压成一行：logo 名字 套餐 / 最紧窗口的细条和倒计时 / 剩余%
static void draw_row(const card_t *c, int x, int y, int w, int h, int64_t age_s)
{
    fb_rbox(x, y, w, h, 5, C_BORDER.r, C_BORDER.g, C_BORDER.b, 255);
    fb_rbox(x + 1, y + 1, w - 2, h - 2, 4, C_CARD.r, C_CARD.g, C_CARD.b, 255);
    rgb_t accent = accent_of(c->id);
    const mask_t *logo = logo_of(c->id);
    bool two = h >= 26;  // 够高就分两行：名字在上，细条在下
    int ty = two ? y + 3 : y + (h - 15) / 2;
    if (logo) fb_mask(x + 7, two ? y + (h - logo->h) / 2 : ty, logo->w, logo->h, logo->a, accent.r, accent.g, accent.b, 1.0f);
    int nw = text_align(&font_label, x + 28, ty, ALIGN_LEFT, c->name, 0, C_TEXT.r, C_TEXT.g, C_TEXT.b, 255);

    char val[28];
    rgb_t vc = C_TEXT;
    int crit = -1;
    if (c->nwin > 0) {
        crit = 0;
        for (int k = 1; k < c->nwin; k++) {
            if (c->win[k].used > c->win[crit].used) crit = k;
        }
        int left = 100 - c->win[crit].used;
        snprintf(val, sizeof(val), "%d%%", left);
        vc = level_color(left);
    } else {
        snprintf(val, sizeof(val), "%s", c->meta[0] ? c->meta : "--");
        if (!c->meta[0]) vc = C_DIM;
    }
    int vw = text_align(&font_num, x + w - 9, y + (h - 22) / 2, ALIGN_RIGHT, val, 0, vc.r, vc.g, vc.b, 255);

    if (c->plan[0]) {
        char plan[16];
        snprintf(plan, sizeof(plan), "%s", c->plan);
        text_fit(&font_small, plan, sizeof(plan), w - 28 - nw - 6 - vw - 16);
        text_plain(&font_small, x + 34 + nw, ty + 2, plan, 0, C_DIM.r, C_DIM.g, C_DIM.b, 255);
    }
    if (crit >= 0 && two) {
        const win_t *wn = &c->win[crit];
        int left = 100 - wn->used;
        char eta[16];
        win_eta(wn, age_s, eta, sizeof(eta));
        char lab[28];
        snprintf(lab, sizeof(lab), "%s %s", wn->key, eta);
        int bx = x + 28, bw = w - 28 - vw - 9 - 12 - 62;
        bar(bx, y + h - 9, bw, 4, left, level_color(left));
        text_plain(&font_tiny, bx + bw + 5, y + h - 11, lab, 0, C_DIM.r, C_DIM.g, C_DIM.b, 255);
    } else if (c->nwin == 0 && c->meta[0] && two) {
        text_plain(&font_tiny, x + 28, y + h - 11, "balance", 0, C_DIM.r, C_DIM.g, C_DIM.b, 255);
    }
}

static void quota_enter(void)
{
    for (int y = 0; y < GFX_H; y++) gfx_set_bg_row(y, 11, 14, 19);
}

static void quota_frame(float dt, int64_t now_ms)
{
    (void)dt;
    gfx_compose(GFX_H);

    int64_t age_s = s_snap.valid ? (now_ms - s_snap.recv_ms) / 1000 : 0;
    if (age_s < 0) age_s = 0;

    text_draw(&font_label, PAD + 3, 3, s_snap.valid ? s_snap.clock : "--:--", C_TEXT.r, C_TEXT.g, C_TEXT.b, 255);
    char st[24];
    rgb_t sc;
    if (!s_snap.valid) {
        snprintf(st, sizeof(st), "NO DATA");
        sc = C_DIM;
    } else if (s_snap.bye) {
        snprintf(st, sizeof(st), "MAC STOPPED");
        sc = C_DIM;
    } else if (age_s <= 5) {
        snprintf(st, sizeof(st), "LINK \xC2\xB7 %llds", (long long)age_s);
        sc = C_OK;
    } else if (age_s <= 120) {
        snprintf(st, sizeof(st), "STALE %llds", (long long)age_s);
        sc = C_WARN;
    } else {
        snprintf(st, sizeof(st), "NO LINK %lldm", (long long)(age_s / 60));
        sc = C_BAD;
    }
    text_align(&font_tiny, GFX_W - PAD - 3, 7, ALIGN_RIGHT, st, 1, sc.r, sc.g, sc.b, 255);

    if (!s_snap.valid || s_snap.ncard == 0) {
        text_align(&font_label, GFX_W / 2, 100, ALIGN_CENTER, "AI QUOTA", 3, C_TEXT.r, C_TEXT.g, C_TEXT.b, 255);
        text_align(&font_small, GFX_W / 2, 124, ALIGN_CENTER, "waiting for the mac", 0, C_DIM.r, C_DIM.g, C_DIM.b, 255);
        return;
    }

    // 卡片按自然高度等比分掉可用高度，最后一张吃掉取整误差
    int sum = 0, nat[MAX_CARDS];
    for (int i = 0; i < s_snap.ncard; i++) {
        nat[i] = natural_h(&s_snap.card[i]);
        sum += nat[i];
    }
    int top = HEADER_H + 3;
    int avail = GFX_H - top - PAD - GAP * (s_snap.ncard - 1);
    int y = top, used = 0;
    if (sum > avail) {  // 完整卡片放不下：每家一行，等高
        for (int i = 0; i < s_snap.ncard; i++) {
            int h = i == s_snap.ncard - 1 ? avail - used : avail / s_snap.ncard;
            draw_row(&s_snap.card[i], PAD, y, GFX_W - 2 * PAD, h, age_s);
            used += h;
            y += h + GAP;
        }
        return;
    }
    for (int i = 0; i < s_snap.ncard; i++) {
        int h = i == s_snap.ncard - 1 ? avail - used : (sum > 0 ? nat[i] * avail / sum : avail / s_snap.ncard);
        draw_card(&s_snap.card[i], PAD, y, GFX_W - 2 * PAD, h, age_s);
        used += h;
        y += h + GAP;
    }
}

// ============================================================ 接入面板

static void quota_init(void)
{
}

static void quota_data(const cJSON *root, int64_t now_ms)
{
    static snapshot_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.valid = true;
    tmp.recv_ms = now_ms;
    json_str(tmp.clock, sizeof(tmp.clock), root, "t");

    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "p")) {
        if (tmp.ncard >= MAX_CARDS) break;
        card_t *c = &tmp.card[tmp.ncard];
        json_str(c->id, sizeof(c->id), it, "id");
        json_str(c->name, sizeof(c->name), it, "n");
        json_str(c->plan, sizeof(c->plan), it, "s");
        json_str(c->meta, sizeof(c->meta), it, "m");
        const cJSON *w = NULL;
        cJSON_ArrayForEach(w, cJSON_GetObjectItemCaseSensitive(it, "w")) {
            if (c->nwin >= MAX_WINDOWS) break;
            win_t *x = &c->win[c->nwin++];
            json_str(x->key, sizeof(x->key), w, "k");
            x->used = (int)clampf((float)json_num(w, "u", 0), 0, 100);
            x->reset_in = (int32_t)json_num(w, "x", -1);
            json_str(x->at, sizeof(x->at), w, "r");
        }
        if (!c->name[0]) {
            memcpy(c->name, c->id, sizeof(c->id));  // id 比 name 短，整块拷过去就是带结尾 0 的
        }
        tmp.ncard++;
    }
    s_snap = tmp;
}

static void quota_bye(void)
{
    s_snap.bye = true;
}

static int quota_badge(int64_t now_ms)
{
    (void)now_ms;
    for (int i = 0; i < s_snap.ncard; i++) {
        if (s_snap.card[i].nwin && critical_left(&s_snap.card[i]) < 20) return 1;
    }
    return 0;
}

// 磁贴：每家一行「logo 名字 细条 剩余%」
static void quota_tile(const tile_t *t, int64_t now_ms)
{
    (void)now_ms;
    int n = s_snap.ncard > 4 ? 4 : s_snap.ncard;
    int rh = n ? (t->h + 2) / n : 0;
    if (rh > 17) rh = 17;
    for (int i = 0; i < n; i++) {
        const card_t *c = &s_snap.card[i];
        int y = t->y + i * rh;
        rgb_t accent = accent_of(c->id);
        const mask_t *logo = logo_of(c->id);
        if (logo) fb_mask(t->x, y, logo->w, logo->h, logo->a, accent.r, accent.g, accent.b, 0.95f);
        text_plain(&font_small, t->x + 20, y + 1, c->name, 0, C_TEXT.r, C_TEXT.g, C_TEXT.b, 230);
        if (c->nwin) {
            int left = critical_left(c);
            rgb_t lc = level_color(left);
            char v[8];
            snprintf(v, sizeof(v), "%d%%", left);
            int vw = text_align(&font_mono, t->x + t->w, y + 2, ALIGN_RIGHT, v, 0, lc.r, lc.g, lc.b, 240);
            int bx = t->x + 74, bw = t->w - 74 - vw - 6;
            if (bw > 8) bar(bx, y + 6, bw, 4, left, lc);
        } else if (c->meta[0]) {
            char m[28];
            snprintf(m, sizeof(m), "%s", c->meta);
            text_fit(&font_mono, m, sizeof(m), t->w - 74);
            text_align(&font_mono, t->x + t->w, y + 2, ALIGN_RIGHT, m, 0, C_TEXT.r, C_TEXT.g, C_TEXT.b, 230);
        }
    }
}

const app_t app_quota = {
    .id = "quota",
    .name = "AI QUOTA",
    .color = {120, 220, 150},
    .init = quota_init,
    .data = quota_data,
    .bye = quota_bye,
    .enter = quota_enter,
    .frame = quota_frame,
    .tile = quota_tile,
    .badge = quota_badge,
};
