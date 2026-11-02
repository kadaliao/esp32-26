#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "board.h"
#include "gfx.h"
#include "link.h"
#include "scene.h"
#include "shot.h"

static const char *TAG = "main";

#define STRIPE_LINES 24
#define FRAME_MS     33   // ~30 fps

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_dma[2];

// 日志走非阻塞写：主机不在线时绝不能让固件卡死
static int log_vprintf(const char *fmt, va_list ap)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0 && usb_serial_jtag_is_driver_installed()) {
        int len = n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1;
        usb_serial_jtag_write_bytes(buf, (size_t)len, 0);
    }
    return n;
}

// 推屏与渲染并行：
//   渲染任务（核 1）画完一帧后，边换字节边拷进 PSRAM 里的 s_front，然后立刻去画下一帧；
//   推屏任务（核 0）把 s_front 分条拷进两块 DMA 缓冲乒乓发给屏幕。
// ST7789 要大端 RGB565，fb 是小端，换字节就在这次拷贝里顺手做掉。
static uint16_t *s_front;
static SemaphoreHandle_t s_go, s_idle;

static void push_task(void *arg)
{
    (void)arg;
    while (1) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        int pending = 0;
        for (int y = 0, i = 0; y < GFX_H; y += STRIPE_LINES, i ^= 1) {
            if (pending == 2) {
                board_wait_flush();
                pending--;
            }
            int lines = GFX_H - y < STRIPE_LINES ? GFX_H - y : STRIPE_LINES;
            memcpy(s_dma[i], s_front + y * GFX_W, (size_t)lines * GFX_W * 2);
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, GFX_W, y + lines, s_dma[i]);
            pending++;
        }
        while (pending--) {
            board_wait_flush();
        }
        xSemaphoreGive(s_idle);
    }
}

static void lcd_push(void)
{
    xSemaphoreTake(s_idle, portMAX_DELAY);  // 等上一帧发完
    const uint32_t *src = (const uint32_t *)gfx_fb;
    uint32_t *dst = (uint32_t *)s_front;
    for (int k = 0; k < GFX_W * GFX_H / 2; k++) {
        uint32_t p = src[k];
        dst[k] = ((p & 0x00FF00FFu) << 8) | ((p >> 8) & 0x00FF00FFu);
    }
    xSemaphoreGive(s_go);
}

static void render_task(void *arg)
{
    (void)arg;
    int64_t last = esp_timer_get_time();
    int64_t stat_t = last;
    int frames = 0;
    int64_t busy_us = 0;
    int64_t push_us = 0;

    while (1) {
        int64_t t0 = esp_timer_get_time();
        float dt = (t0 - last) / 1e6f;
        last = t0;
        if (dt > 0.1f) {
            dt = 0.1f;
        }

        snapshot_t s;
        if (link_take_snapshot(&s)) {
            scene_update(&s);
        }
        scene_frame(dt, t0 / 1000);
        int64_t t1 = esp_timer_get_time();
        lcd_push();
        push_us += esp_timer_get_time() - t1;

        if (link_shot_requested()) {
            shot_send();
        }
        uint8_t bright;
        if (link_take_brightness(&bright)) {
            board_backlight(bright);
        }

        int64_t used = esp_timer_get_time() - t0;
        busy_us += used;
        frames++;
        if (t0 - stat_t > 10 * 1000000) {
            ESP_LOGW(TAG, "%.1f fps, %lld us/frame (push %lld)", frames * 1e6f / (t0 - stat_t),
                     busy_us / frames, push_us / frames);
            push_us = 0;
            stat_t = t0;
            frames = 0;
            busy_us = 0;
        }
        int wait = FRAME_MS - (int)(used / 1000);
        vTaskDelay(pdMS_TO_TICKS(wait > 1 ? wait : 1));
    }
}

void app_main(void)
{
    esp_log_set_vprintf(log_vprintf);
    link_start();  // 先装串口驱动，后面的日志才写得出去

    s_panel = board_init();

    for (int i = 0; i < 2; i++) {
        s_dma[i] = heap_caps_malloc(GFX_W * STRIPE_LINES * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_dma[i]) {
            ESP_LOGE(TAG, "no dma buffer");
            return;
        }
    }
    if (!gfx_init()) {
        ESP_LOGE(TAG, "no frame buffer");
        return;
    }
    s_front = heap_caps_malloc(GFX_W * GFX_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_front) {
        ESP_LOGE(TAG, "no front buffer");
        return;
    }
    s_go = xSemaphoreCreateBinary();
    s_idle = xSemaphoreCreateBinary();
    xSemaphoreGive(s_idle);
    scene_init();
    xTaskCreatePinnedToCore(push_task, "push", 4096, NULL, 6, NULL, 0);

    // 渲染放在核 1，串口收包在核 0
    xTaskCreatePinnedToCore(render_task, "render", 8192, NULL, 5, NULL, 1);
}
