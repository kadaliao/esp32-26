#pragma once

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
