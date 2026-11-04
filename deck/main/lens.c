#include "lens.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "esp_camera.h"
#include "face.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cam";

// 立创·实战派 ESP32-S3 的 DVP 引脚（和小智 lichuang-dev 一致）。
// SCCB 不另开总线：复用 board.c 建好的 I2C1（触摸、IO 扩展也在上面），驱动的 I2C 总线锁会排队。
#define PIN_XCLK  5
#define PIN_PCLK  7
#define PIN_VSYNC 3
#define PIN_HREF  46
#define PIN_D0    16
#define PIN_D1    18
#define PIN_D2    8
#define PIN_D3    17
#define PIN_D4    15
#define PIN_D5    6
#define PIN_D6    4
#define PIN_D7    9
#define XCLK_HZ   24000000

// 画面方向：镜子要左右反过来（你举右手，画面里也是右边那只手动）
#define CAM_HMIRROR 1
#define CAM_VFLIP   0

_Static_assert(FACE_W == LENS_MW && FACE_H == LENS_MH, "人脸检测的收件箱就是半分辨率图");

#define STOP_AFTER_MISSES 3  // 连着几次取不到帧就当摄像头坏了

typedef struct {
    uint16_t *img;
    uint8_t *luma;
    uint8_t *motion;
    int moving;
    uint32_t seq;
} slot_t;

// 两个读者（渲染线程、转发到 Mac 的任务）各占一块，一块是最新写好的，cam 任务总能写剩下那块
#define READERS 2
#define SLOTS   (READERS + 2)
static slot_t s_slot[SLOTS];
static int s_ready = -1, s_reading[READERS] = {-1, -1};
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_seq;
static uint8_t *s_prev_luma;
static bool s_have_prev;

static TaskHandle_t s_task;
static volatile bool s_want;
static volatile lens_state_t s_state = LENS_OFF;
static char s_err[48];
static volatile float s_fps;

static bool power_up(void)
{
    s_state = LENS_STARTING;
    board_camera_power(true);
    vTaskDelay(pdMS_TO_TICKS(20));

    camera_config_t cfg = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = PIN_XCLK,
        .pin_sccb_sda = -1,  // -1 = 用 sccb_i2c_port 那条已经初始化好的总线
        .pin_sccb_scl = -1,
        .sccb_i2c_port = 1,
        .pin_d7 = PIN_D7,
        .pin_d6 = PIN_D6,
        .pin_d5 = PIN_D5,
        .pin_d4 = PIN_D4,
        .pin_d3 = PIN_D3,
        .pin_d2 = PIN_D2,
        .pin_d1 = PIN_D1,
        .pin_d0 = PIN_D0,
        .pin_vsync = PIN_VSYNC,
        .pin_href = PIN_HREF,
        .pin_pclk = PIN_PCLK,
        .xclk_freq_hz = XCLK_HZ,
        .ledc_timer = LEDC_TIMER_2,      // S3 上 XCLK 由摄像头外设自己出，这两个用不上；
        .ledc_channel = LEDC_CHANNEL_2,  // 写成和背光（TIMER_1 / CHANNEL_0）不同的，以防万一
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = FRAMESIZE_QVGA,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        snprintf(s_err, sizeof(s_err), "init: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_err);
        board_camera_power(false);
        s_state = LENS_FAILED;
        return false;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_hmirror(s, CAM_HMIRROR);
        s->set_vflip(s, CAM_VFLIP);
    }
    s_have_prev = false;
    ESP_LOGW(TAG, "camera up (pid 0x%x) in %lld ms, internal free %u", s ? s->id.PID : 0,
             (esp_timer_get_time() - t0) / 1000, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

static void power_down(void)
{
    esp_camera_deinit();
    board_camera_power(false);
    s_fps = 0;
    ESP_LOGW(TAG, "camera off");
}

// 传感器出的是大端 RGB565。一趟走完：换成小端写进 img，顺手算 2×2 平均的亮度和帧差；
// 人脸检测器空闲的话，再顺手写一张 2×2 平均的半分辨率彩色图给它
static void process(const camera_fb_t *fb, slot_t *s)
{
    const uint32_t *src = (const uint32_t *)fb->buf;
    uint16_t *small = face_inbox();
    int moving = 0;
    for (int my = 0; my < LENS_MH; my++) {
        const uint32_t *r0 = src + (2 * my) * (LENS_W / 2);
        const uint32_t *r1 = r0 + LENS_W / 2;
        uint32_t *d0 = (uint32_t *)(s->img + 2 * my * LENS_W);
        uint32_t *d1 = d0 + LENS_W / 2;
        uint8_t *lu = s->luma + my * LENS_MW;
        uint8_t *mo = s->motion + my * LENS_MW;
        const uint8_t *pv = s_prev_luma + my * LENS_MW;
        for (int mx = 0; mx < LENS_MW; mx++) {
            // 一个 32 位字是同一行相邻两个像素，交换每个 16 位里的两个字节
            uint32_t a = r0[mx], b = r1[mx];
            a = ((a & 0x00FF00FFu) << 8) | ((a >> 8) & 0x00FF00FFu);
            b = ((b & 0x00FF00FFu) << 8) | ((b >> 8) & 0x00FF00FFu);
            d0[mx] = a;
            d1[mx] = b;
            // 四个像素的 R5、G6、B5 各自求和，再一次算亮度（0.30R + 0.59G + 0.11B）
            int R = (int)((a >> 11) & 31) + (int)(a >> 27) + (int)((b >> 11) & 31) + (int)(b >> 27);
            int G = (int)((a >> 5) & 63) + (int)((a >> 21) & 63) + (int)((b >> 5) & 63) + (int)((b >> 21) & 63);
            int B = (int)(a & 31) + (int)((a >> 16) & 31) + (int)(b & 31) + (int)((b >> 16) & 31);
            int y = (R * 616 + G * 600 + B * 232) >> 10;
            if (small) small[my * LENS_MW + mx] = (uint16_t)(((R >> 2) << 11) | ((G >> 2) << 5) | (B >> 2));
            lu[mx] = (uint8_t)y;
            int d = s_have_prev ? abs(y - pv[mx]) : 0;
            mo[mx] = (uint8_t)d;
            moving += d > LENS_MOTION_T;
        }
    }
    memcpy(s_prev_luma, s->luma, LENS_MW * LENS_MH);
    s_have_prev = true;
    s->moving = moving;
    if (small) face_submit();
}

static int pick_slot(void)
{
    int w = 0;
    taskENTER_CRITICAL(&s_mux);
    while (w == s_ready || w == s_reading[0] || w == s_reading[1]) w++;
    taskEXIT_CRITICAL(&s_mux);
    return w;
}

static void cam_task(void *arg)
{
    (void)arg;
    bool on = false;
    int misses = 0, frames = 0, stats = 0;
    int64_t stat_t = 0;
    while (1) {
        if (s_want && !on && s_state != LENS_FAILED) {
            on = power_up();
            misses = frames = 0;
            stat_t = esp_timer_get_time();
        }
        if (!s_want && on) {
            power_down();
            on = false;
        }
        if (!on) {
            if (!s_want) s_state = LENS_OFF;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // 等 lens_start / lens_stop
            continue;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            if (++misses >= STOP_AFTER_MISSES) {
                snprintf(s_err, sizeof(s_err), "no frames from sensor");
                ESP_LOGE(TAG, "%s", s_err);
                power_down();
                on = false;
                s_state = LENS_FAILED;
            }
            continue;
        }
        misses = 0;
        if (fb->width == LENS_W && fb->height == LENS_H && fb->len >= LENS_W * LENS_H * 2) {
            int w = pick_slot();
            process(fb, &s_slot[w]);
            taskENTER_CRITICAL(&s_mux);
            s_slot[w].seq = ++s_seq;
            s_ready = w;
            taskEXIT_CRITICAL(&s_mux);
            s_state = LENS_ON;
            frames++;
        }
        esp_camera_fb_return(fb);

        int64_t now = esp_timer_get_time();
        if (now - stat_t > 2000000) {
            s_fps = frames * 1e6f / (float)(now - stat_t);
            frames = 0;
            stat_t = now;
            if (++stats % 5 == 0) {  // 和渲染的帧率日志一样，10 秒一行
                ESP_LOGW(TAG, "%.1f fps, %d moving", s_fps, s_slot[s_ready >= 0 ? s_ready : 0].moving);
            }
        }
    }
}

void lens_init(void)
{
    for (int i = 0; i < SLOTS; i++) {
        // 16 字节对齐：JPEG 编码器（送去 Mac）要求输入这样对齐
        s_slot[i].img = heap_caps_aligned_alloc(16, LENS_W * LENS_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_slot[i].luma = heap_caps_malloc(LENS_MW * LENS_MH, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_slot[i].motion = heap_caps_malloc(LENS_MW * LENS_MH, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_slot[i].img || !s_slot[i].luma || !s_slot[i].motion) {
            snprintf(s_err, sizeof(s_err), "out of memory");
            s_state = LENS_FAILED;
            return;
        }
    }
    s_prev_luma = heap_caps_malloc(LENS_MW * LENS_MH, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_prev_luma) {
        snprintf(s_err, sizeof(s_err), "out of memory");
        s_state = LENS_FAILED;
        return;
    }
    // 核 0：和推屏、串口收包同核，渲染独占核 1
    xTaskCreatePinnedToCore(cam_task, "cam", 4096, NULL, 4, &s_task, 0);
}

void lens_start(void)
{
    if (!s_task) return;
    taskENTER_CRITICAL(&s_mux);
    s_ready = s_reading[0] = s_reading[1] = -1;  // 上次留下的旧画面不要
    taskEXIT_CRITICAL(&s_mux);
    if (s_state == LENS_FAILED) s_state = LENS_OFF;  // 失败过的话再试一次
    s_want = true;
    xTaskNotifyGive(s_task);
}

void lens_stop(void)
{
    if (!s_task) return;
    s_want = false;
    xTaskNotifyGive(s_task);
}

lens_state_t lens_state(void)
{
    return s_state;
}

const char *lens_error(void)
{
    return s_err;
}

float lens_fps(void)
{
    return s_fps;
}

bool lens_latest(lens_frame_t *out)
{
    return lens_latest_for(LENS_READER_RENDER, out);
}

bool lens_latest_for(int reader, lens_frame_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    if (s_ready >= 0) s_reading[reader] = s_ready;
    int r = s_reading[reader];
    taskEXIT_CRITICAL(&s_mux);
    if (r < 0) return false;
    const slot_t *s = &s_slot[r];
    out->img = s->img;
    out->luma = s->luma;
    out->motion = s->motion;
    out->moving = s->moving;
    out->seq = s->seq;
    return true;
}
