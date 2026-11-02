#pragma once

#include <stdbool.h>
#include <stdint.h>

// 主机每 250ms 推一帧：此刻在飞的请求 + 上一帧以来刚落地的请求
#define MAX_FLY   12
#define MAX_DONE  8

typedef struct {
    char id[16];      // 请求 id（网关里的序号，或日志里的 request id）
    char agent[16];   // 发起方 agent id：claude / codex / dsh ...
    char aname[20];   // 显示名
    char prov[20];    // provider id
    char pname[20];   // provider 显示名
    char model[24];
    char aicon[9];    // 图标 key（icons.c 缓存里查）
    char picon[9];
    char phase[12];   // upstream / queued / ...（仅在飞）
    int32_t ms;       // 已飞 / 总耗时（毫秒）
    int32_t out;      // 输出 token（仅落地）
    bool ok;          // 仅落地
} req_t;

typedef struct {
    bool valid;
    int64_t recv_ms;
    char clock[8];
    int32_t calls;       // 今天的请求数
    float cost;          // 今天的花费（美元）
    bool bye;            // 主机说它要退出了
    bool gw_down;        // 主机连不上 magpie 网关
    uint8_t nfly, ndone;
    req_t fly[MAX_FLY];
    req_t done[MAX_DONE];
} snapshot_t;
