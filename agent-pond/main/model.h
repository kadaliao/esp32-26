#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MAX_AGENTS 8

typedef enum {
    ST_TOOL = 0,   // 正在调用工具
    ST_THINK,      // 在想 / 在生成
    ST_WAIT,       // 等你回话
    ST_SLEEP,      // 很久没动静
} agent_state_t;

typedef struct {
    char id[12];
    char kind[16];   // claude / codex / gemini / ...
    char name[24];   // 项目目录名
    char tool[16];   // 当前工具名（仅 ST_TOOL）
    uint8_t state;
    uint8_t kids;    // 正在跑的子 agent 数
    int32_t age;     // 距上次动静的秒数
} agent_info_t;

typedef struct {
    bool valid;
    int64_t recv_ms;
    char clock[8];
    uint8_t n;
    agent_info_t a[MAX_AGENTS];
} snapshot_t;
