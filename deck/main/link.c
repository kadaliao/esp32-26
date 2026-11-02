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

static QueueHandle_t s_q;
static volatile int64_t s_last_rx;
static volatile bool s_shot;
static volatile int s_bright = -1; // -1 = 无待处理请求；默认 0 会在开机时把背光关掉

// ---------------------------------------------------------------- 串口线程

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

void link_write(const void *data, size_t len, uint32_t timeout_ms)
{
    const uint8_t *p = (const uint8_t *)data;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (len > 0) {
        int w = usb_serial_jtag_write_bytes(p, len, pdMS_TO_TICKS(20));
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
