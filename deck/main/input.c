#include "input.h"

#include <stdlib.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "input";

#define TOUCH_ADDR    0x38   // FT6336
#define PIN_BOOT      0      // BOOT 键，按下为低
#define POLL_MS       10
#define LONG_MS       600
#define SWIPE_PX      40
#define MOVE_PX       12     // 挪动超过这么多就不算点击了
#define UP_DEBOUNCE   3      // 连续这么多次读不到手指才算抬起（芯片偶尔漏一帧）

// 触摸芯片按竖屏 240x320 报坐标，屏幕是横过来用的。映射参照小智 lichuang-dev 的
// mirror_x + swap_xy；如果实测方向反了，改这里就行。
static void map_xy(int rx, int ry, int *x, int *y)
{
    *x = ry;
    *y = 239 - rx;
}

static i2c_master_dev_handle_t s_tp;
static QueueHandle_t s_q;
static volatile bool s_down;
static volatile int16_t s_cx, s_cy;

static bool tp_read(int *x, int *y)
{
    uint8_t reg = 0x02, buf[5];
    if (i2c_master_transmit_receive(s_tp, &reg, 1, buf, sizeof(buf), 20) != ESP_OK) {
        return false;
    }
    int n = buf[0] & 0x0F;
    if (n == 0 || n > 2) {
        return false;
    }
    if ((buf[1] >> 6) == 1) {  // 事件标志 01 = 抬起
        return false;
    }
    int rx = ((buf[1] & 0x0F) << 8) | buf[2];
    int ry = ((buf[3] & 0x0F) << 8) | buf[4];
    map_xy(rx, ry, x, y);
    if (*x < 0) *x = 0;
    if (*x > 319) *x = 319;
    if (*y < 0) *y = 0;
    if (*y > 239) *y = 239;
    return true;
}

static void push(ev_type_t t, int x, int y)
{
    input_ev_t e = {t, (int16_t)x, (int16_t)y};
    xQueueSend(s_q, &e, 0);
}

static void input_task(void *arg)
{
    (void)arg;
    bool down = false, long_sent = false, moved = false;
    int sx = 0, sy = 0, lx = 0, ly = 0, up_cnt = 0;
    int64_t t0 = 0;

    bool btn = false, btn_long = false;
    int64_t bt0 = 0;

    while (1) {
        int64_t now = esp_timer_get_time() / 1000;

        // ---- 触摸 ----
        int x, y;
        bool touch = s_tp && tp_read(&x, &y);
        if (touch) {
            up_cnt = 0;
            if (!down) {
                down = true;
                sx = lx = x;
                sy = ly = y;
                t0 = now;
                long_sent = moved = false;
            }
            lx = x;
            ly = y;
            if (abs(x - sx) > MOVE_PX || abs(y - sy) > MOVE_PX) {
                moved = true;
            }
            if (!moved && !long_sent && now - t0 >= LONG_MS) {
                push(EV_LONG, sx, sy);
                long_sent = true;
            }
            s_cx = (int16_t)x;
            s_cy = (int16_t)y;
            s_down = true;
        } else if (down && ++up_cnt >= UP_DEBOUNCE) {
            down = false;
            s_down = false;
            int dx = lx - sx, dy = ly - sy;
            if (long_sent) {
                // 长按已经发过了，抬手不再算别的
            } else if (abs(dx) >= SWIPE_PX && abs(dx) * 10 > abs(dy) * 13) {
                push(dx < 0 ? EV_SWIPE_LEFT : EV_SWIPE_RIGHT, sx, sy);
            } else if (abs(dy) >= SWIPE_PX && abs(dy) * 10 > abs(dx) * 13) {
                push(dy < 0 ? EV_SWIPE_UP : EV_SWIPE_DOWN, sx, sy);
            } else if (!moved) {
                push(EV_TAP, sx, sy);
            }
        }

        // ---- BOOT 键 ----
        bool pressed = gpio_get_level(PIN_BOOT) == 0;
        if (pressed && !btn) {
            btn = true;
            btn_long = false;
            bt0 = now;
        } else if (pressed && btn && !btn_long && now - bt0 >= LONG_MS) {
            btn_long = true;
            push(EV_BTN_LONG, 0, 0);
        } else if (!pressed && btn) {
            btn = false;
            if (!btn_long && now - bt0 >= 30) {  // 30ms 以下当抖动
                push(EV_BTN, 0, 0);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void input_start(void)
{
    s_q = xQueueCreate(16, sizeof(input_ev_t));

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    i2c_master_bus_handle_t bus = board_i2c();
    if (bus && i2c_master_probe(bus, TOUCH_ADDR, 50) == ESP_OK) {
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = TOUCH_ADDR,
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(bus, &dev, &s_tp) != ESP_OK) {
            s_tp = NULL;
        }
    }
    ESP_LOGW(TAG, "touch %s", s_tp ? "ft6336 @0x38" : "not found, BOOT key only");

    xTaskCreatePinnedToCore(input_task, "input", 3072, NULL, 4, NULL, 0);
}

bool input_has_touch(void)
{
    return s_tp != NULL;
}

bool input_take(input_ev_t *ev)
{
    return s_q && xQueueReceive(s_q, ev, 0) == pdTRUE;
}

bool input_touching(int *x, int *y)
{
    if (!s_down) {
        return false;
    }
    *x = s_cx;
    *y = s_cy;
    return true;
}
