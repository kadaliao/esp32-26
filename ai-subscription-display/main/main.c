#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "driver/usb_serial_jtag.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "board.h"
#include "link.h"
#include "shot.h"
#include "ui.h"

static const char *TAG = "main";

#define BUF_LINES  40
#define BUF_PIXELS (LCD_H_RES * BUF_LINES)

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_buf[2];

// 日志走非阻塞写：主机不在线时绝不能让固件卡死
static int log_vprintf(const char *fmt, va_list ap)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    // 驱动没装好之前不能用 write_bytes（会解引用空指针），直接丢弃
    if (n > 0 && usb_serial_jtag_is_driver_installed()) {
        int len = n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1;
        usb_serial_jtag_write_bytes(buf, (size_t)len, 0);
    }
    return n;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;

    // 同步一份整屏影子缓冲，供串口截图使用（保持 LVGL 原生的 16 位小端）
    int sw = 0;
    int sh = 0;
    const uint16_t *shadow = ui_shadow(&sw, &sh);
    if (shadow && area->x1 >= 0 && area->y1 >= 0 && area->x2 < sw && area->y2 < sh) {
        uint16_t *dst = (uint16_t *)shadow;
        for (int y = 0; y < h; y++) {
            memcpy(dst + (size_t)(area->y1 + y) * sw + area->x1, px_map + (size_t)y * w * 2,
                   (size_t)w * 2);
        }
    }

    // ST7789 要高位在前（大端），而 ESP32 是小端，所以必须先把 RGB565 字节交换再发。
    // 少了这一步，画面位置是对的但颜色全乱——表现为“看得见、看不清”。
    lv_draw_sw_rgb565_swap(px_map, (uint32_t)w * (uint32_t)h);

    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    board_wait_flush(); // 等 DMA 发完才能复用这块绘制缓冲
    lv_display_flush_ready(disp);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    int64_t last_tick = 0;

    while (1) {
        uint32_t delay = lv_timer_handler();

        snapshot_t s;
        if (link_take_snapshot(&s)) {
            ui_update(&s);
        }

        int64_t now = esp_timer_get_time() / 1000;
        if (now - last_tick >= 1000) {
            last_tick = now;
            ui_tick();
        }

        if (link_shot_requested()) {
            shot_send();
        }

        uint8_t bright;
        if (link_take_brightness(&bright)) {
            board_backlight(bright);
        }

        if (delay < 5) {
            delay = 5;
        } else if (delay > 30) {
            delay = 30;
        }
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

void app_main(void)
{
    esp_log_set_vprintf(log_vprintf);

    // 先把串口驱动装好，后面所有日志才写得了
    link_start();

    s_panel = board_init();

    for (int i = 0; i < 2; i++) {
        s_buf[i] = heap_caps_malloc(BUF_PIXELS * sizeof(uint16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!s_buf[i]) {
            s_buf[i] = heap_caps_malloc(BUF_PIXELS * sizeof(uint16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (!s_buf[i]) {
            ESP_LOGE(TAG, "no draw buffer %d", i);
            return;
        }
    }

    lv_init();
    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, s_buf[0], s_buf[1], BUF_PIXELS * sizeof(uint16_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    ui_init();

    xTaskCreate(lvgl_task, "lvgl", 8192, disp, 5, NULL);
    ESP_LOGI(TAG, "started");
}
