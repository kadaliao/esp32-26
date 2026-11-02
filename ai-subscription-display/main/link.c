#include "link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

static const char *TAG = "link";

#define LINK_LINE_MAX 4096
#define RX_CHUNK      256

static SemaphoreHandle_t s_lock;
static snapshot_t s_snap;
static bool s_dirty;
static volatile bool s_shot;
static volatile int s_bright = -1; // -1 = 无待处理请求；默认 0 会在开机时把背光关掉

static void copy_str(char *dst, size_t cap, const cJSON *item, const char *field)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(item, field);
    dst[0] = 0;
    if (cJSON_IsString(v) && v->valuestring) {
        snprintf(dst, cap, "%s", v->valuestring);
    }
}

static void parse_payload(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "bad json");
        return;
    }

    snapshot_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.valid = true;
    tmp.recv_ms = esp_timer_get_time() / 1000;

    const cJSON *clock = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (cJSON_IsString(clock) && clock->valuestring) {
        snprintf(tmp.clock, sizeof(tmp.clock), "%s", clock->valuestring);
    }

    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "p");
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (tmp.ncard >= MAX_CARDS) {
            break;
        }
        card_t *c = &tmp.card[tmp.ncard];
        copy_str(c->id, sizeof(c->id), it, "id");
        copy_str(c->name, sizeof(c->name), it, "n");
        copy_str(c->plan, sizeof(c->plan), it, "s");
        copy_str(c->meta, sizeof(c->meta), it, "m");

        const cJSON *wins = cJSON_GetObjectItemCaseSensitive(it, "w");
        const cJSON *w = NULL;
        cJSON_ArrayForEach(w, wins) {
            if (c->nwin >= MAX_WINDOWS) {
                break;
            }
            win_t *x = &c->win[c->nwin];
            copy_str(x->key, sizeof(x->key), w, "k");
            const cJSON *u = cJSON_GetObjectItemCaseSensitive(w, "u");
            const cJSON *rx = cJSON_GetObjectItemCaseSensitive(w, "x");
            x->used = cJSON_IsNumber(u) ? u->valueint : 0;
            if (x->used < 0) {
                x->used = 0;
            }
            if (x->used > 100) {
                x->used = 100;
            }
            x->reset_in = cJSON_IsNumber(rx) ? (int32_t)rx->valuedouble : -1;
            copy_str(x->at, sizeof(x->at), w, "r");
            c->nwin++;
        }
        if (c->name[0] == 0) {
            char fallback[sizeof(c->id)];
            memcpy(fallback, c->id, sizeof(fallback));
            fallback[sizeof(fallback) - 1] = 0;
            snprintf(c->name, sizeof(c->name), "%s", fallback);
        }
        tmp.ncard++;
    }
    cJSON_Delete(root);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snap = tmp;
    s_dirty = true;
    xSemaphoreGive(s_lock);
}

static void handle_line(char *line)
{
    if (strncmp(line, "@@", 2) != 0) {
        return;
    }
    const char *cmd = line + 2;

    if (cmd[0] == '{') {
        parse_payload(cmd);
    } else if (strcmp(cmd, "PING") == 0) {
        link_write("!!PONG\n", 7, 20);
    } else if (strcmp(cmd, "SHOT") == 0) {
        s_shot = true;
    } else if (strncmp(cmd, "BRIGHT", 6) == 0) {
        s_bright = atoi(cmd + 6);
        link_write("!!OK\n", 5, 20);
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

bool link_take_snapshot(snapshot_t *out)
{
    bool got = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_dirty) {
        *out = s_snap;
        s_dirty = false;
        got = true;
    }
    xSemaphoreGive(s_lock);
    return got;
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
    s_lock = xSemaphoreCreateMutex();

    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 4096,
        .rx_buffer_size = 8192,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    // 让 printf / ESP_LOG 也走这个驱动，避免和 RX 抢同一个外设
    usb_serial_jtag_vfs_use_driver();

    xTaskCreate(rx_task, "link_rx", 4096, NULL, 9, NULL);
    ESP_LOGI(TAG, "usb-serial-jtag up");
}
