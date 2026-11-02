#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MAX_CARDS   6
#define MAX_WINDOWS 3

typedef struct {
    char key[8];       // "5h" / "7d"
    int used;          // 已用百分比 0..100
    int32_t reset_in;  // 距离重置的秒数，-1 表示未知
    char at[12];       // 重置的本地时刻，如 "at 22:59" / "Tue 09:00"，空 = 未知
} win_t;

typedef struct {
    char id[10];       // claude / codex / deepseek / grok
    char name[20];     // 显示名
    char plan[14];     // 套餐，如 Max / Pro
    char meta[28];     // 余额等附加信息
    int nwin;
    win_t win[MAX_WINDOWS];
} card_t;

typedef struct {
    bool valid;
    int64_t recv_ms;    // 收到时刻（esp_timer 毫秒）
    char clock[9];      // "15:42:07"
    int ncard;
    card_t card[MAX_CARDS];
} snapshot_t;

static inline int card_left(const card_t *c)
{
    return 100 - c->win[0].used;
}
