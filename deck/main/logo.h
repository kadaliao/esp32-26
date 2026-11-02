#pragma once
#include <stdint.h>

// RGB565 + 8 位 alpha 的小图（scripts/gen-logo.py 从 magpie.app 的图标生成）
typedef struct {
    uint8_t w, h;
    const uint16_t *px;
    const uint8_t *a;
} image_t;

extern const image_t logo_s;   // 20px：标题栏
extern const image_t logo_m;   // 30px：网关中心
extern const image_t logo_l;   // 72px：开机画面
