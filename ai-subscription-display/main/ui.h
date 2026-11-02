#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "model.h"

// 建好屏幕骨架（空状态）
void ui_init(void);

// 用新快照刷新界面
void ui_update(const snapshot_t *s);

// 每秒调用：推进倒计时、"多久没更新"提示
void ui_tick(void);

// 上一帧的整屏像素（RGB565，LVGL 原生顺序），供截图使用
const uint16_t *ui_shadow(int *w, int *h);
