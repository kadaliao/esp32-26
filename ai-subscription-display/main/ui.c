#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "board.h"
#include "fonts.h"
#include "logos.h"

static const char *TAG = "ui";

// ---- 配色 ----
#define C_BG      0x0B0E13
#define C_CARD    0x151A22
#define C_BORDER  0x242C38
#define C_TEXT    0xE6EDF3
#define C_DIM     0x8B949E
#define C_TRACK   0x2E3742
#define C_OK      0x3FB950
#define C_WARN    0xD29922
#define C_BAD     0xF85149

// ---- 尺寸 ----
#define HEADER_H  20
#define PAD       5
#define GAP       4
#define CARD_W    (LCD_H_RES - 2 * PAD)

// 卡片内部行高：自然高度必须挤进可用高度，否则缩放后内容会溢出、
// 最后一张卡（Grok）会被底部裁掉。
#define CARD_PAD  4
#define TITLE_H   18
#define ROW_H     13
#define META_H    11
#define BAR_H     6

typedef struct {
    lv_obj_t *card;
    lv_obj_t *logo;
    lv_obj_t *name;
    lv_obj_t *plan;
    lv_obj_t *value;
    lv_obj_t *meta;
    lv_obj_t *key[MAX_WINDOWS];
    lv_obj_t *bar[MAX_WINDOWS];
    lv_obj_t *eta[MAX_WINDOWS];
} card_ui_t;

static card_ui_t s_ui[MAX_CARDS];
static lv_obj_t *s_clock;
static lv_obj_t *s_status;
static lv_obj_t *s_splash;
static snapshot_t s_cur;
static uint16_t *s_shadow;

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const lv_image_dsc_t *logo_of(const char *id)
{
    if (strcmp(id, "claude") == 0) {
        return &logo_claude;
    }
    if (strcmp(id, "codex") == 0) {
        return &logo_codex;
    }
    if (strcmp(id, "deepseek") == 0) {
        return &logo_deepseek;
    }
    if (strcmp(id, "grok") == 0) {
        return &logo_grok;
    }
    return NULL;
}

static lv_color_t accent_of(const char *id)
{
    if (strcmp(id, "claude") == 0) {
        return lv_color_hex(0xD97757);
    }
    if (strcmp(id, "codex") == 0) {
        return lv_color_hex(0x00A67E);
    }
    if (strcmp(id, "deepseek") == 0) {
        return lv_color_hex(0x4D6BFE);
    }
    if (strcmp(id, "grok") == 0) {
        return lv_color_hex(0xC9D1D9);
    }
    return lv_color_hex(C_DIM);
}

static lv_color_t level_color(int left)
{
    if (left >= 50) {
        return lv_color_hex(C_OK);
    }
    if (left >= 20) {
        return lv_color_hex(C_WARN);
    }
    return lv_color_hex(C_BAD);
}

static void fmt_eta(int32_t secs, char *buf, size_t cap)
{
    if (secs < 0) {
        snprintf(buf, cap, "--");
    } else if (secs <= 0) {
        snprintf(buf, cap, "now");
    } else if (secs >= 86400) {
        snprintf(buf, cap, "%dd %dh", (int)(secs / 86400), (int)((secs % 86400) / 3600));
    } else if (secs >= 3600) {
        snprintf(buf, cap, "%dh%02dm", (int)(secs / 3600), (int)((secs % 3600) / 60));
    } else {
        snprintf(buf, cap, "%dm", (int)(secs / 60) > 0 ? (int)(secs / 60) : 1);
    }
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

void ui_init(void)
{
    lv_tick_set_cb(tick_cb);

    s_shadow = heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_shadow) {
        s_shadow = heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_shadow) {
        memset(s_shadow, 0, LCD_H_RES * LCD_V_RES * sizeof(uint16_t));
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_clock = mk_label(scr, &lv_font_yen_14, C_TEXT);
    lv_obj_align(s_clock, LV_ALIGN_TOP_LEFT, PAD + 3, 3);
    lv_label_set_text(s_clock, "--:--");

    s_status = mk_label(scr, &lv_font_yen_10, C_DIM);
    lv_obj_set_width(s_status, 140);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -(PAD + 3), 5);
    lv_label_set_text(s_status, "NO DATA");

    for (int i = 0; i < MAX_CARDS; i++) {
        card_ui_t *u = &s_ui[i];
        u->card = lv_obj_create(scr);
        lv_obj_set_style_bg_color(u->card, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(u->card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(u->card, 6, 0);
        lv_obj_set_style_border_width(u->card, 1, 0);
        lv_obj_set_style_border_color(u->card, lv_color_hex(C_BORDER), 0);
        lv_obj_set_style_pad_all(u->card, 0, 0);
        lv_obj_remove_flag(u->card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(u->card, LV_OBJ_FLAG_HIDDEN);

        // A8 单色 logo：图片只有透明度，颜色由 image_recolor 给（100% 覆盖）
        u->logo = lv_image_create(u->card);
        lv_obj_set_style_image_recolor_opa(u->logo, LV_OPA_COVER, 0);

        u->name = mk_label(u->card, &lv_font_yen_14, C_TEXT);
        u->plan = mk_label(u->card, &lv_font_yen_10, C_DIM);
        u->value = mk_label(u->card, &lv_font_yen_16, C_TEXT);
        u->meta = mk_label(u->card, &lv_font_yen_10, C_DIM);

        for (int k = 0; k < MAX_WINDOWS; k++) {
            u->key[k] = mk_label(u->card, &lv_font_yen_10, C_DIM);
            u->eta[k] = mk_label(u->card, &lv_font_yen_10, C_DIM);
            lv_obj_set_width(u->eta[k], 56);
            lv_obj_set_style_text_align(u->eta[k], LV_TEXT_ALIGN_RIGHT, 0);

            u->bar[k] = lv_bar_create(u->card);
            lv_obj_set_style_bg_color(u->bar[k], lv_color_hex(C_TRACK), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(u->bar[k], LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(u->bar[k], 0, LV_PART_MAIN);
            lv_obj_set_style_radius(u->bar[k], 4, LV_PART_MAIN);
            lv_obj_set_style_radius(u->bar[k], 4, LV_PART_INDICATOR);
            lv_bar_set_range(u->bar[k], 0, 100);
            lv_obj_add_flag(u->bar[k], LV_OBJ_FLAG_HIDDEN);
        }
    }

    s_splash = lv_obj_create(scr);
    lv_obj_set_size(s_splash, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(s_splash, 0, 0);
    lv_obj_set_style_bg_color(s_splash, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_splash, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_splash, 0, 0);
    lv_obj_set_style_pad_all(s_splash, 0, 0);
    lv_obj_remove_flag(s_splash, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t1 = mk_label(s_splash, &lv_font_yen_16, C_TEXT);
    lv_label_set_text(t1, "AI QUOTA PANEL");
    lv_obj_align(t1, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *t2 = mk_label(s_splash, &lv_font_yen_10, C_DIM);
    lv_label_set_text(t2, "waiting for host  \xC2\xB7  usb-serial-jtag");
    lv_obj_align(t2, LV_ALIGN_CENTER, 0, 12);

    ESP_LOGI(TAG, "ui ready");
}

static int natural_h(const card_t *c)
{
    int h = CARD_PAD * 2 + TITLE_H;
    h += c->nwin * ROW_H;
    if (c->meta[0]) {
        h += META_H;
    }
    return h;
}

static void layout_cards(const snapshot_t *s)
{
    int sum = 0;
    int nat[MAX_CARDS] = {0};
    for (int i = 0; i < s->ncard; i++) {
        nat[i] = natural_h(&s->card[i]);
        sum += nat[i];
    }
    int body_top = HEADER_H + 3;
    int avail = LCD_V_RES - body_top - PAD - GAP * (s->ncard > 0 ? s->ncard - 1 : 0);
    int y = body_top;
    int used = 0;

    for (int i = 0; i < s->ncard; i++) {
        int h;
        if (i == s->ncard - 1) {
            // 最后一张吃掉取整误差（注意 only 高度参与分配，gap 不算）
            h = avail - used;
        } else {
            h = (sum > 0) ? (nat[i] * avail / sum) : avail / s->ncard;
        }
        lv_obj_set_pos(s_ui[i].card, PAD, y);
        lv_obj_set_size(s_ui[i].card, CARD_W, h);
        lv_obj_remove_flag(s_ui[i].card, LV_OBJ_FLAG_HIDDEN);
        used += h;
        y += h + GAP;
    }

    for (int i = s->ncard; i < MAX_CARDS; i++) {
        lv_obj_add_flag(s_ui[i].card, LV_OBJ_FLAG_HIDDEN);
    }
}

static void fill_card(int idx, const card_t *c)
{
    card_ui_t *u = &s_ui[idx];
    lv_obj_update_layout(u->card);
    int cw = lv_obj_get_width(u->card);

    lv_color_t accent = accent_of(c->id);
    lv_obj_set_style_image_recolor(u->logo, accent, 0);
    const lv_image_dsc_t *logo = logo_of(c->id);
    if (logo) {
        lv_image_set_src(u->logo, logo);
        lv_obj_remove_flag(u->logo, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(u->logo, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_pos(u->logo, 8, CARD_PAD + (TITLE_H - LOGO_SIZE) / 2);

    lv_label_set_text(u->name, c->name);
    lv_obj_set_pos(u->name, 8 + LOGO_SIZE + 5, CARD_PAD - 1);

    lv_label_set_text(u->plan, c->plan);
    lv_obj_align_to(u->plan, u->name, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    int critical = 100;
    for (int k = 0; k < c->nwin; k++) {
        int left = 100 - c->win[k].used;
        if (left < critical) {
            critical = left;
        }
    }

    if (c->nwin > 0) {
        lv_label_set_text_fmt(u->value, "%d%%", critical);
        lv_obj_set_style_text_color(u->value, level_color(critical), 0);
    } else if (c->meta[0]) {
        lv_label_set_text(u->value, c->meta);
        lv_obj_set_style_text_color(u->value, lv_color_hex(C_TEXT), 0);
    } else {
        lv_label_set_text(u->value, "--");
        lv_obj_set_style_text_color(u->value, lv_color_hex(C_DIM), 0);
    }
    lv_obj_update_layout(u->value);
    lv_obj_set_pos(u->value, cw - 10 - lv_obj_get_width(u->value), CARD_PAD - 2);

    for (int k = 0; k < MAX_WINDOWS; k++) {
        if (k < c->nwin) {
            int y = CARD_PAD + TITLE_H + k * ROW_H;
            int left = 100 - c->win[k].used;

            lv_obj_remove_flag(u->bar[k], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(u->bar[k], 30, y + 3);
            lv_obj_set_size(u->bar[k], cw - 30 - 10 - 58, BAR_H);
            lv_bar_set_value(u->bar[k], left, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(u->bar[k], level_color(left), LV_PART_INDICATOR);

            lv_label_set_text(u->key[k], c->win[k].key);
            lv_obj_set_pos(u->key[k], 10, y);

            lv_obj_set_pos(u->eta[k], cw - 10 - 56, y);
        } else {
            lv_obj_add_flag(u->bar[k], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(u->key[k], "");
            lv_label_set_text(u->eta[k], "");
        }
    }

    if (c->meta[0]) {
        lv_label_set_text(u->meta, c->nwin == 0 ? "balance" : c->meta);
        lv_obj_set_pos(u->meta, 10, CARD_PAD + TITLE_H + c->nwin * ROW_H);
    } else {
        lv_label_set_text(u->meta, "");
    }
}

static void refresh_times(void)
{
    if (!s_cur.valid) {
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    int64_t age = (now - s_cur.recv_ms) / 1000;
    if (age < 0) {
        age = 0;
    }

    for (int i = 0; i < s_cur.ncard; i++) {
        for (int k = 0; k < s_cur.card[i].nwin; k++) {
            const win_t *w = &s_cur.card[i].win[k];
            int32_t remain = w->reset_in;
            if (remain >= 0) {
                remain -= (int32_t)age;
            }
            char buf[16];
            if (w->used >= 100 && w->at[0] && remain > 0) {
                // 用满了，关心的是“几点能用”，倒计时反而要心算
                snprintf(buf, sizeof(buf), "%s", w->at);
            } else {
                fmt_eta(remain, buf, sizeof(buf));
            }
            lv_label_set_text(s_ui[i].eta[k], buf);
        }
    }
}

void ui_update(const snapshot_t *s)
{
    s_cur = *s;
    if (!s->valid) {
        return;
    }
    if (s_splash) {
        lv_obj_add_flag(s_splash, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_clock, s->clock);

    layout_cards(s);
    lv_obj_update_layout(lv_screen_active());

    for (int i = 0; i < s->ncard; i++) {
        fill_card(i, &s->card[i]);
    }
    refresh_times();
}

void ui_tick(void)
{
    if (!s_cur.valid) {
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    int64_t age = (now - s_cur.recv_ms) / 1000;
    if (age < 0) {
        age = 0;
    }

    if (age <= 5) {
        lv_label_set_text_fmt(s_status, "LINK \xC2\xB7 %llds", (long long)age);
        lv_obj_set_style_text_color(s_status, lv_color_hex(C_OK), 0);
    } else if (age <= 120) {
        lv_label_set_text_fmt(s_status, "STALE %llds", (long long)age);
        lv_obj_set_style_text_color(s_status, lv_color_hex(C_WARN), 0);
    } else {
        lv_label_set_text_fmt(s_status, "NO LINK %lldm", (long long)(age / 60));
        lv_obj_set_style_text_color(s_status, lv_color_hex(C_BAD), 0);
    }
    refresh_times();
}

const uint16_t *ui_shadow(int *w, int *h)
{
    if (w) {
        *w = LCD_H_RES;
    }
    if (h) {
        *h = LCD_V_RES;
    }
    return s_shadow;
}
