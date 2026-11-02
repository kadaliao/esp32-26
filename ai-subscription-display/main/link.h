#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "model.h"

// 串口链路：Mac 端每 2 秒推一行 "@@{json}\n"，设备解析后交给 UI。
void link_start(void);

// 取最新快照；自上次调用以来有新数据则返回 true
bool link_take_snapshot(snapshot_t *out);

// 写串口（非阻塞优先，带超时）
void link_write(const void *data, size_t len, uint32_t timeout_ms);
void link_printf(const char *fmt, ...);

// 主机请求截图
bool link_shot_requested(void);

// 亮度命令（0..100），有值时返回 true 并取出
bool link_take_brightness(uint8_t *percent);
