#include "link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "icons.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

static const char *TAG = "link";

#define LINK_LINE_MAX 8192
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

static void parse_req(req_t *r, const cJSON *it)
{
    copy_str(r->id, sizeof(r->id), it, "id");
    copy_str(r->agent, sizeof(r->agent), it, "a");
    copy_str(r->aname, sizeof(r->aname), it, "an");
    copy_str(r->prov, sizeof(r->prov), it, "p");
    copy_str(r->pname, sizeof(r->pname), it, "pn");
    copy_str(r->model, sizeof(r->model), it, "m");
    copy_str(r->phase, sizeof(r->phase), it, "ph");
    copy_str(r->aicon, sizeof(r->aicon), it, "ai");
    copy_str(r->picon, sizeof(r->picon), it, "pi");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(it, "ms");
    r->ms = cJSON_IsNumber(v) ? (int32_t)v->valuedouble : 0;
    v = cJSON_GetObjectItemCaseSensitive(it, "o");
    r->out = cJSON_IsNumber(v) ? (int32_t)v->valuedouble : 0;
    v = cJSON_GetObjectItemCaseSensitive(it, "ok");
    r->ok = !cJSON_IsNumber(v) || v->valueint != 0;
}

static void parse_payload(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "bad json");
        return;
    }

    static snapshot_t tmp;  // 太大，不放 rx 任务的栈上
    memset(&tmp, 0, sizeof(tmp));
    tmp.valid = true;
    tmp.recv_ms = esp_timer_get_time() / 1000;

    const cJSON *clock = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (cJSON_IsString(clock) && clock->valuestring) {
        snprintf(tmp.clock, sizeof(tmp.clock), "%s", clock->valuestring);
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "n");
    tmp.calls = cJSON_IsNumber(v) ? (int32_t)v->valuedouble : 0;
    v = cJSON_GetObjectItemCaseSensitive(root, "c");
    tmp.cost = cJSON_IsNumber(v) ? (float)v->valuedouble : 0;
    v = cJSON_GetObjectItemCaseSensitive(root, "x");
    tmp.gw_down = cJSON_IsNumber(v) && v->valueint != 0;

    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "f")) {
        if (tmp.nfly >= MAX_FLY) break;
        parse_req(&tmp.fly[tmp.nfly], it);
        if (tmp.fly[tmp.nfly].id[0]) tmp.nfly++;
    }
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "d")) {
        if (tmp.ndone >= MAX_DONE) break;
        parse_req(&tmp.done[tmp.ndone], it);
        if (tmp.done[tmp.ndone].id[0]) tmp.ndone++;
    }
    cJSON_Delete(root);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    // 上一帧还没被取走时，把它的「落地」事件并进来，免得丢
    if (s_dirty) {
        for (int i = 0; i < s_snap.ndone && tmp.ndone < MAX_DONE; i++) {
            tmp.done[tmp.ndone++] = s_snap.done[i];
        }
    }
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
    } else if (strncmp(cmd, "ICON ", 5) == 0) {
        // @@ICON <key> <尺寸> <hex>
        char key[9] = {0};
        int px = 0, off = 0;
        if (sscanf(cmd + 5, "%8s %d %n", key, &px, &off) == 2 && px == ICON_PX) {
            icons_put(key, cmd + 5 + off);
        }
    } else if (strcmp(cmd, "BYE") == 0) {
        // 主机退出：发一帧空快照，在飞的鸟都收回来，画面切到「已停止」
        static snapshot_t bye;
        memset(&bye, 0, sizeof(bye));
        bye.valid = true;
        bye.bye = true;
        bye.recv_ms = esp_timer_get_time() / 1000;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(bye.clock, sizeof(bye.clock), "%s", s_snap.clock);
        bye.calls = s_snap.calls;
        bye.cost = s_snap.cost;
        s_snap = bye;
        s_dirty = true;
        xSemaphoreGive(s_lock);
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
        .rx_buffer_size = 16384,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    // 让 printf / ESP_LOG 也走这个驱动，避免和 RX 抢同一个外设
    usb_serial_jtag_vfs_use_driver();

    xTaskCreate(rx_task, "link_rx", 4096, NULL, 9, NULL);
    ESP_LOGI(TAG, "usb-serial-jtag up");
}
