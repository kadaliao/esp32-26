#pragma once
#include <stdint.h>

// 单色 logo 遮罩（scripts/gen-logos.sh 从 assets/logos/*.svg 生成），画的时候按品牌色着色
typedef struct {
    uint8_t w, h;
    const uint8_t *a;
} mask_t;

extern const mask_t logo_claude;
extern const mask_t logo_codex;
extern const mask_t logo_deepseek;
extern const mask_t logo_grok;
