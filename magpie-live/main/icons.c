#include "icons.h"

#include <stdio.h>
#include <string.h>

#define MAX_ICONS 24

typedef struct {
    char key[9];
    uint16_t px[ICON_PX * ICON_PX];
    uint8_t a[ICON_PX * ICON_PX];
    image_t img;
    uint32_t stamp;
} slot_t;

static slot_t s_icons[MAX_ICONS];
static uint32_t s_clock;

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int byte_at(const char *h, int i)
{
    int hi = hexv(h[i * 2]), lo = hexv(h[i * 2 + 1]);
    return hi < 0 || lo < 0 ? -1 : (hi << 4) | lo;
}

void icons_put(const char *key, const char *hex)
{
    const int n = ICON_PX * ICON_PX;
    if (strlen(hex) < (size_t)n * 6) return;

    // 同 key 覆盖；否则用空位；满了挤掉最久没用的
    slot_t *s = NULL, *old = &s_icons[0];
    for (int i = 0; i < MAX_ICONS; i++) {
        if (strcmp(s_icons[i].key, key) == 0) s = &s_icons[i];
        if (s_icons[i].stamp < old->stamp) old = &s_icons[i];
    }
    if (!s) s = old;

    // 先解到临时区，整块无误再换上，免得画到一半的图
    static uint16_t px[ICON_PX * ICON_PX];
    static uint8_t a[ICON_PX * ICON_PX];
    for (int i = 0; i < n; i++) {
        int lo = byte_at(hex, i * 3), hi = byte_at(hex, i * 3 + 1), al = byte_at(hex, i * 3 + 2);
        if (lo < 0 || hi < 0 || al < 0) return;
        px[i] = (uint16_t)(lo | (hi << 8));
        a[i] = (uint8_t)al;
    }
    s->key[0] = 0;  // 写的过程中先让它查不到
    memcpy(s->px, px, sizeof(px));
    memcpy(s->a, a, sizeof(a));
    s->img = (image_t){ICON_PX, ICON_PX, s->px, s->a};
    s->stamp = ++s_clock;
    snprintf(s->key, sizeof(s->key), "%s", key);
}

const image_t *icons_get(const char *key)
{
    if (!key[0]) return NULL;
    for (int i = 0; i < MAX_ICONS; i++) {
        if (s_icons[i].key[0] && strcmp(s_icons[i].key, key) == 0) {
            s_icons[i].stamp = ++s_clock;
            return &s_icons[i].img;
        }
    }
    return NULL;
}
