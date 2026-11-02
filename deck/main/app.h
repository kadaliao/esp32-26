#pragma once
// 面板上的每个小应用都实现这一张表，再在 shell.c 的 APPS[] 里登记一行就能出现在首页。
//
// 数据流：主机发 "@@<id>{json}"（id 就是下面的 id 字段）→ 串口线程排队 →
// 渲染线程每帧取出来，解析成 cJSON 交给 data()。不在前台的 app 也会收到 data()，
// 所以首页磁贴和角标随时是新的；只有前台的 app 才会被调 frame()。

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "input.h"
#include "util.h"

// 首页磁贴的可画区域（已经扣掉了标题行）
typedef struct {
    int x, y, w, h;
    float t;        // 秒，做动画用
} tile_t;

typedef struct {
    const char *id;     // 协议名，主机发 "@@<id>{...}"
    const char *name;   // 首页磁贴标题
    rgb_t color;        // 主题色：磁贴边框、角标、切换时的页码点

    void (*init)(void);                               // 开机调一次
    void (*data)(const cJSON *root, int64_t now_ms);  // 主机来了新数据
    void (*bye)(void);                                // 主机说它要退出了（可选）
    void (*enter)(void);                              // 切到前台：设背景、清掉过期的动画状态（可选）
    void (*leave)(void);                              // 切走（可选）
    void (*frame)(float dt, int64_t now_ms);          // 前台时每帧调，负责画满整屏
    void (*tile)(const tile_t *t, int64_t now_ms);    // 首页磁贴里的迷你预览（可选）
    bool (*input)(const input_ev_t *ev);              // 手势，返回 true 表示自己处理了，壳就不再管（可选）
    int (*badge)(int64_t now_ms);                     // 0 平静 / 1 有动静 / 2 需要你看一眼（可选）
} app_t;

extern const app_t app_sys;
extern const app_t app_magpie;
extern const app_t app_pond;
extern const app_t app_quota;
