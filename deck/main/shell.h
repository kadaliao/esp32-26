#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

// 面板的「壳」：首页磁贴、手势切换、切换动画、各 app 的数据分发
void shell_init(void);
void shell_frame(float dt, int64_t now_ms);   // 处理手势并画好整屏

// 以下由 link.c 在渲染线程里调
void shell_data(const char *id, const cJSON *root, int64_t now_ms);
void shell_bye(void);
void shell_go(const char *id);                // "home" 或某个 app 的 id

// 当前画面的 id（"home" 或 app id）。任何线程都能读
const char *shell_current(void);

// 背光由壳管：主机设的亮度（@@BRIGHT）是上限；Mac 睡了 / 主机走了、又没人碰屏幕时自动调暗
void shell_brightness(uint8_t percent);
bool shell_idle(void);  // 正暗着：渲染可以降帧
void shell_wake(int64_t now_ms);  // 算作有人在（和碰了一下屏幕一样推迟调暗、暗着就亮回来）

// 当前 app 想要的帧率（0 = 默认 30）。画面本来就更新得慢的 app（比如等 Mac 画的 GHIBLI）可以要低一点，
// 省下的 CPU 和 PSRAM 带宽留给别的活；切换动画、点击水波纹时壳会自动回到 30fps。切 app 时清零
void shell_want_fps(int fps);
int shell_frame_ms(void);  // 主循环用：这一帧该隔多久
