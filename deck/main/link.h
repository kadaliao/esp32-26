#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 串口链路（USB-Serial/JTAG），主机 → 板子按行：
//   @@<app>{json}    给某个 app 的数据，例如 @@sys{...}、@@magpie{...}
//   @@{json}         老格式（单项目时代的主机脚本），按字段猜是哪个 app
//   @@ICON k px hex  共享图标缓存
//   @@GO <app>|home  切屏
//   @@BYE            主机退出
//   @@HELLO / @@PING / @@SHOT / @@BRIGHT n
//   @@BIN <tag> <len> 后面紧跟 len 个原始字节（二进制帧，比如 Mac 处理好的画面）
// 板子 → 主机：!!HELLO deck <版本> <当前 app> / !!APP <id> / !!IN <手势> / !!PONG / !!OK / !!SHOT…
//   !!BIN <tag> <len> + 原始字节（比如送去 Mac 处理的摄像头画面）
void link_start(void);

// 渲染线程每帧调一次：把排队的消息交给对应的 app
void link_poll(int64_t now_ms);

// 最近一次收到主机消息的时刻（毫秒），从没收到过是 0
int64_t link_last_rx_ms(void);

// 写串口（非阻塞优先，带超时）
void link_write(const void *data, size_t len, uint32_t timeout_ms);
void link_printf(const char *fmt, ...);
// 发一个二进制帧（头和数据中间不会插进别的输出）；拿不到锁或超时返回 false
bool link_write_bin(const char *tag, const void *data, size_t len, uint32_t timeout_ms);
// 日志专用：串口正忙（在发二进制帧）就直接丢掉这行，绝不阻塞
void link_log_write(const char *s, size_t len);

// 收二进制帧：主机发来 tag 对应的帧时，在串口线程里调 cb（数据只在回调期间有效，要留就自己拷）
typedef void (*link_bin_cb)(const uint8_t *data, size_t len);
void link_on_bin(const char *tag, link_bin_cb cb);

// 主机请求截图
bool link_shot_requested(void);

// 亮度命令（0..100），有值时返回 true 并取出
bool link_take_brightness(uint8_t *percent);
