#pragma once

#include <stdbool.h>
#include <stdint.h>

// 触摸屏（FT6336，和 IO 扩展芯片同一条 I2C）+ BOOT 键，识别成手势事件
typedef enum {
    EV_NONE = 0,
    EV_TAP,          // 点一下，x/y 是位置
    EV_LONG,         // 按住不动 0.6 秒（手指还没抬起就会发）
    EV_SWIPE_LEFT,   // 手指往左划
    EV_SWIPE_RIGHT,
    EV_SWIPE_UP,
    EV_SWIPE_DOWN,
    EV_BTN,          // BOOT 键短按
    EV_BTN_LONG,     // BOOT 键长按
} ev_type_t;

typedef struct {
    ev_type_t type;
    int16_t x, y;    // 起点（屏幕坐标，320x240 横屏）
} input_ev_t;

void input_start(void);
bool input_has_touch(void);           // 探测到触摸芯片了吗
bool input_take(input_ev_t *ev);      // 取一个事件，没有返回 false
bool input_touching(int *x, int *y);  // 手指此刻是否按在屏幕上（按下高亮用）
