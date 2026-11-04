#pragma once
// 把摄像头画面送到 Mac 处理（比如吉卜力风格），再把处理好的画面收回来显示。
// 板子 → Mac：!!BIN cam <len> + JPEG；Mac → 板子：@@BIN fx <len> + JPEG（320×240）。
// 编码在核 0、解码在核 1（优先级比渲染低）；最多同时 2 帧在路上，Mac 慢了就少送，不会越积越多。

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float fps;          // 最近每秒收回几帧
    float latency_ms;   // 一帧从送出到收回要多久
    int64_t last_ms;    // 最近一次收到处理好的画面，没收到过是 0
    int sent_kb;        // 最近一帧送出去多大
    float enc_ms;       // 板子上编一帧 JPEG 要多久
    float dec_ms;       // 解一帧要多久
} relay_stats_t;

void relay_init(void);    // 开机调一次
void relay_start(void);   // 开始送帧（摄像头要另外 lens_start）
void relay_stop(void);
bool relay_active(void);

// 渲染线程用：最近一张处理好的画面（320×240 RGB565 小端），没有返回 false。在下次调用前一直有效
bool relay_latest(const uint16_t **img, uint32_t *seq);
void relay_stats(relay_stats_t *out);
