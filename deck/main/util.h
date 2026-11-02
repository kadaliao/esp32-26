#pragma once
// 各个 app 都要用的小工具：颜色、插值、随机数、取 JSON 字段

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#define PI  3.1415927f
#define PI2 6.2831853f

typedef struct {
    uint8_t r, g, b;
} rgb_t;

#define RGB(r, g, b) ((rgb_t){(r), (g), (b)})

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// 指数趋近：每秒按 rate 的速度靠向 target，和帧率无关
static inline float approach(float cur, float target, float rate, float dt)
{
    return cur + (target - cur) * (1.0f - expf(-rate * dt));
}

static inline rgb_t mix(rgb_t a, rgb_t b, float t)
{
    return (rgb_t){(uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
                   (uint8_t)(a.b + (b.b - a.b) * t)};
}

static inline float ease_out(float t)
{
    t = clampf(t, 0, 1);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

static inline uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

// 每个 .c 各有一份随机数状态，互不干扰
static uint32_t util_rng = 0x9E3779B9u;

static inline uint32_t rnd(void)
{
    util_rng ^= util_rng << 13;
    util_rng ^= util_rng >> 17;
    util_rng ^= util_rng << 5;
    return util_rng;
}

static inline float frand(float lo, float hi)
{
    return lo + (hi - lo) * (float)(rnd() & 0xFFFFFF) / (float)0xFFFFFF;
}

// ---- JSON：字段缺了或类型不对就给默认值 ----

static inline void json_str(char *dst, size_t cap, const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    dst[0] = 0;
    if (cJSON_IsString(v) && v->valuestring) {
        snprintf(dst, cap, "%s", v->valuestring);
    }
}

static inline double json_num(const cJSON *obj, const char *key, double dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

// 主题色里常用的几种
static const rgb_t C_WHITE = {255, 255, 255};
static const rgb_t C_GOLD = {255, 206, 120};
static const rgb_t C_AMBER = {255, 196, 110};
static const rgb_t C_RED = {255, 84, 84};
static const rgb_t C_CYAN = {0, 234, 255};
static const rgb_t C_PINK = {255, 122, 217};
