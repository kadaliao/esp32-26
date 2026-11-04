#include "link.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "icons.h"
#include "input.h"
#include "shell.h"

static const char *TAG = "link";

#define LINK_LINE_MAX 8192
#define RX_CHUNK      256
#define QUEUE_LEN     24
#define DRAIN_MAX     16   // 每帧最多处理几条，免得一次积压把一帧拖得太长

#define BIN_MAX       (160 * 1024)  // 一个二进制帧最大多少（一张 320×240 的 JPEG 绰绰有余）
#define MAX_BIN_TAGS  4

static QueueHandle_t s_q;
static SemaphoreHandle_t s_wlock;  // 写串口的锁：一个二进制帧的头和数据中间不能插进日志

// 二进制帧：主机发 "@@BIN <tag> <len>\n" 后面紧跟 len 个原始字节
static struct {
    char tag[12];
    link_bin_cb cb;
} s_bin_tags[MAX_BIN_TAGS];
static uint8_t *s_bin;      // 收帧缓冲（PSRAM）
static size_t s_bin_len, s_bin_got;
static int s_bin_who = -1;  // 收完交给谁；-1 = 没人要，收完扔掉
static bool s_bin_on;       // 正在收二进制数据
static volatile int64_t s_last_rx;
static volatile bool s_shot;
static volatile int s_bright = -1; // -1 = 无待处理请求；默认 0 会在开机时把背光关掉

// ---------------------------------------------------------------- 串口线程

static void link_write_raw(const void *data, size_t len, uint32_t timeout_ms);

static void enqueue(const char *cmd)
{
    size_t n = strlen(cmd);
    char *m = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m) {
        m = malloc(n + 1);
    }
    if (!m) {
        return;
    }
    memcpy(m, cmd, n + 1);
    if (xQueueSend(s_q, &m, 0) != pdTRUE) {
        // 渲染线程跟不上（比如正在发截图）：丢最旧的那条
        char *old = NULL;
        if (xQueueReceive(s_q, &old, 0) == pdTRUE) {
            free(old);
        }
        if (xQueueSend(s_q, &m, 0) != pdTRUE) {
            free(m);
        }
    }
}

static void handle_line(char *line)
{
    if (strncmp(line, "@@", 2) != 0) {
        return;
    }
    const char *cmd = line + 2;
    s_last_rx = esp_timer_get_time() / 1000;

    if (strncmp(cmd, "BIN ", 4) == 0) {
        char tag[12] = {0};
        unsigned len = 0;
        if (sscanf(cmd + 4, "%11s %u", tag, &len) != 2 || len == 0) return;
        s_bin_who = -1;
        for (int i = 0; i < MAX_BIN_TAGS; i++) {
            if (s_bin_tags[i].cb && strcmp(s_bin_tags[i].tag, tag) == 0) s_bin_who = i;
        }
        if (strcmp(tag, "sink") == 0) s_bin_who = -2;  // 测速：收完回一句
        if (len > BIN_MAX || !s_bin) s_bin_who = -1;
        s_bin_len = len;
        s_bin_got = 0;
        s_bin_on = true;
        return;
    }
    if (strncmp(cmd, "BURST ", 6) == 0) {  // 测速：板子往主机发这么多字节
        unsigned len = (unsigned)atoi(cmd + 6);
        static uint8_t chunk[1024];
        for (int i = 0; i < (int)sizeof(chunk); i++) chunk[i] = (uint8_t)i;
        char head[40];
        int hn = snprintf(head, sizeof(head), "!!BIN burst %u\n", len);
        xSemaphoreTake(s_wlock, portMAX_DELAY);
        int64_t t0 = esp_timer_get_time();
        link_write_raw(head, (size_t)hn, 1000);
        for (unsigned sent = 0; sent < len; sent += sizeof(chunk)) {
            unsigned n = len - sent < sizeof(chunk) ? len - sent : sizeof(chunk);
            link_write_raw(chunk, n, 1000);
        }
        int64_t t1 = esp_timer_get_time();
        xSemaphoreGive(s_wlock);
        link_printf("!!BURSTED %u %lld\n", len, (t1 - t0) / 1000);
        return;
    }
    if (strcmp(cmd, "PING") == 0) {
        link_write("!!PONG\n", 7, 20);
    } else if (strcmp(cmd, "HELLO") == 0) {
        link_printf("!!HELLO deck 1 %s touch=%d\n", shell_current(), input_has_touch() ? 1 : 0);
    } else if (strcmp(cmd, "SHOT") == 0) {
        s_shot = true;
    } else if (strncmp(cmd, "BRIGHT", 6) == 0) {
        s_bright = atoi(cmd + 6);
        link_write("!!OK\n", 5, 20);
    } else if (strncmp(cmd, "ICON ", 5) == 0) {
        // @@ICON <key> <尺寸> <hex>：直接进缓存，不用排队
        char key[9] = {0};
        int px = 0, off = 0;
        if (sscanf(cmd + 5, "%8s %d %n", key, &px, &off) == 2 && px == ICON_PX) {
            icons_put(key, cmd + 5 + off);
        }
    } else {
        enqueue(cmd);  // app 数据、BYE、GO：交给渲染线程，和画面状态在同一个线程里改
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    static char line[LINK_LINE_MAX];
    size_t n = 0;
    uint8_t buf[RX_CHUNK];

    while (1) {
        int r = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < r; i++) {
            if (s_bin_on) {  // 二进制帧的数据部分：整段拷，不看换行
                size_t take = s_bin_len - s_bin_got;
                if (take > (size_t)(r - i)) take = (size_t)(r - i);
                if (s_bin_who != -1) memcpy(s_bin + s_bin_got, buf + i, take);
                s_bin_got += take;
                i += (int)take - 1;
                if (s_bin_got == s_bin_len) {
                    s_bin_on = false;
                    s_last_rx = esp_timer_get_time() / 1000;
                    if (s_bin_who >= 0) {
                        s_bin_tags[s_bin_who].cb(s_bin, s_bin_len);
                    } else if (s_bin_who == -2) {
                        link_printf("!!SINK %u\n", (unsigned)s_bin_len);
                    }
                }
                continue;
            }
            char ch = (char)buf[i];
            if (ch == '\n' || ch == '\r') {
                if (n > 0) {
                    line[n] = 0;
                    handle_line(line);
                    n = 0;
                }
            } else if (n < sizeof(line) - 1) {
                line[n++] = ch;
            } else {
                n = 0; // 超长行直接丢弃，等下一个换行重新同步
            }
        }
    }
}

// ---------------------------------------------------------------- 渲染线程

// 老格式没写 app 名：按各个项目原来的字段认
static const char *sniff(const cJSON *root)
{
    if (cJSON_HasObjectItem(root, "a")) return "pond";
    if (cJSON_HasObjectItem(root, "f") || cJSON_HasObjectItem(root, "d")) return "magpie";
    if (cJSON_HasObjectItem(root, "p")) return "quota";
    if (cJSON_HasObjectItem(root, "ne")) return "sys";
    return "";
}

static void dispatch(const char *m, int64_t now_ms)
{
    if (strcmp(m, "BYE") == 0) {
        shell_bye();
        return;
    }
    if (strncmp(m, "GO ", 3) == 0) {
        shell_go(m + 3);
        return;
    }
    const char *brace = strchr(m, '{');
    if (!brace) {
        return;
    }
    char id[16] = {0};
    size_t n = (size_t)(brace - m);
    if (n >= sizeof(id)) {
        return;
    }
    memcpy(id, m, n);
    while (n > 0 && id[n - 1] == ' ') {
        id[--n] = 0;
    }

    cJSON *root = cJSON_Parse(brace);
    if (!root) {
        ESP_LOGW(TAG, "bad json for '%s'", id);
        return;
    }
    shell_data(id[0] ? id : sniff(root), root, now_ms);
    cJSON_Delete(root);
}

void link_poll(int64_t now_ms)
{
    char *m = NULL;
    for (int i = 0; i < DRAIN_MAX && xQueueReceive(s_q, &m, 0) == pdTRUE; i++) {
        dispatch(m, now_ms);
        free(m);
    }
}

int64_t link_last_rx_ms(void)
{
    return s_last_rx;
}

// ---------------------------------------------------------------- 写

// 不拿锁的写，调用方自己拿着 s_wlock
static void link_write_raw(const void *data, size_t len, uint32_t timeout_ms)
{
    const uint8_t *p = (const uint8_t *)data;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (len > 0) {
        // 一次最多写 1KB：驱动的发送环形缓冲只有 4KB，比它大的一次写永远写不进去（返回 0）
        int w = usb_serial_jtag_write_bytes(p, len > 1024 ? 1024 : len, pdMS_TO_TICKS(20));
        if (w > 0) {
            p += w;
            len -= (size_t)w;
            continue;
        }
        if (xTaskGetTickCount() > deadline) {
            return;
        }
    }
}

void link_write(const void *data, size_t len, uint32_t timeout_ms)
{
    if (xSemaphoreTake(s_wlock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return;
    link_write_raw(data, len, timeout_ms);
    xSemaphoreGive(s_wlock);
}

bool link_write_bin(const char *tag, const void *data, size_t len, uint32_t timeout_ms)
{
    char head[40];
    int hn = snprintf(head, sizeof(head), "!!BIN %s %u\n", tag, (unsigned)len);
    if (xSemaphoreTake(s_wlock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    link_write_raw(head, (size_t)hn, timeout_ms);
    link_write_raw(data, len, timeout_ms);
    xSemaphoreGive(s_wlock);
    return true;
}

void link_log_write(const char *s, size_t len)
{
    if (!s_wlock || xSemaphoreTake(s_wlock, 0) != pdTRUE) return;  // 正在发二进制帧：这行日志不要了
    usb_serial_jtag_write_bytes(s, len, 0);
    xSemaphoreGive(s_wlock);
}

void link_on_bin(const char *tag, link_bin_cb cb)
{
    for (int i = 0; i < MAX_BIN_TAGS; i++) {
        if (!s_bin_tags[i].cb) {
            snprintf(s_bin_tags[i].tag, sizeof(s_bin_tags[i].tag), "%s", tag);
            s_bin_tags[i].cb = cb;
            return;
        }
    }
}

void link_printf(const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        link_write(buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1), 200);
    }
}

bool link_shot_requested(void)
{
    if (s_shot) {
        s_shot = false;
        return true;
    }
    return false;
}

bool link_take_brightness(uint8_t *percent)
{
    int v = s_bright;
    if (v < 0) {
        return false;
    }
    s_bright = -1;
    *percent = (uint8_t)(v > 100 ? 100 : v);
    return true;
}

void link_start(void)
{
    s_q = xQueueCreate(QUEUE_LEN, sizeof(char *));
    s_wlock = xSemaphoreCreateMutex();
    s_bin = heap_caps_malloc(BIN_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 4096,
        .rx_buffer_size = 16384,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    // 让 printf / ESP_LOG 也走这个驱动，避免和 RX 抢同一个外设
    usb_serial_jtag_vfs_use_driver();

    xTaskCreate(rx_task, "link_rx", 4096, NULL, 9, NULL);
    ESP_LOGI(TAG, "usb-serial-jtag up");
}
