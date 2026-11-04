#include "face.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "human_face_detect.hpp"
#include "lens.h"

static const char *TAG = "face";

static uint16_t *s_inbox;
static TaskHandle_t s_task;
static volatile bool s_on, s_busy;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static face_t s_last;
static HumanFaceDetect *s_det;
static int64_t s_last_start_us;

#define MIN_GAP_US 160000  // 两次检测至少隔这么久（约每秒 6 次）：眼睛够用了，省下的带宽留给渲染

static void face_task(void *)
{
    int runs = 0;
    float ms_sum = 0;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_on) {
            if (s_det) {  // 不用了就把模型放掉：它占着一块内部 RAM，摄像头、JPEG 编解码器都要用
                delete s_det;
                s_det = nullptr;
                ESP_LOGW(TAG, "detector released, internal free %u",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            }
            s_busy = false;
            continue;
        }
        if (!s_det) s_det = new HumanFaceDetect();
        dl::image::img_t img = {s_inbox, FACE_W, FACE_H, dl::image::DL_IMAGE_PIX_TYPE_RGB565LE};
        int64_t t0 = esp_timer_get_time();
        s_last_start_us = t0;
        std::list<dl::detect::result_t> &res = s_det->run(img);
        int64_t t1 = esp_timer_get_time();
        static HumanFaceDetect *logged;
        if (logged != s_det) {  // 模型是第一次 run 时才真正加载的
            logged = s_det;
            ESP_LOGW(TAG, "first detection %lld ms; internal free %u (largest %u), psram free %u", (t1 - t0) / 1000,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }

        face_t f = {};
        const float k = (float)LENS_W / FACE_W;  // 收件箱是半分辨率，换回画面坐标
        int best = -1;
        for (const auto &r : res) {
            f.n++;
            if (r.box.size() < 4 || r.box_area() <= best) continue;
            best = r.box_area();
            f.x = r.box[0] * k;
            f.y = r.box[1] * k;
            f.w = (r.box[2] - r.box[0]) * k;
            f.h = (r.box[3] - r.box[1]) * k;
            f.score = r.score;
        }
        f.found = best >= 0;
        f.ms = (t1 - t0) / 1000.0f;
        f.at_ms = t1 / 1000;

        taskENTER_CRITICAL(&s_mux);
        f.seq = s_last.seq + 1;
        bool was = s_last.found;
        s_last = f;
        taskEXIT_CRITICAL(&s_mux);
        if (f.found != was) {
            ESP_LOGW(TAG, "%s (%.0f%%, %.0f px wide)", f.found ? "face" : "no face", f.score * 100, f.w);
        }
        ms_sum += f.ms;
        if (++runs == 100) {
            ESP_LOGW(TAG, "%.1f ms per detection", ms_sum / runs);
            runs = 0;
            ms_sum = 0;
        }
        s_busy = false;
    }
}

void face_init(void)
{
    s_inbox = (uint16_t *)heap_caps_malloc(FACE_W * FACE_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_inbox) {
        ESP_LOGE(TAG, "no inbox");
        return;
    }
    // 核 0、比 cam 任务（4）低：取帧不会被检测拖住，检测只用剩下的时间。ESP-DL 跑起来栈要得多
    xTaskCreatePinnedToCore(face_task, "face", 8192, NULL, 3, &s_task, 0);
}

void face_enable(bool on)
{
    if (on && !s_on) {
        taskENTER_CRITICAL(&s_mux);
        s_last.found = false;  // 上次留下的结果不算数
        s_last.n = 0;
        taskEXIT_CRITICAL(&s_mux);
    }
    s_on = on;
    if (!on && s_task) xTaskNotifyGive(s_task);  // 叫醒它去释放模型
}

bool face_latest(face_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_last;
    taskEXIT_CRITICAL(&s_mux);
    return out->seq != 0;
}

uint16_t *face_inbox(void)
{
    if (!s_task || !s_on || s_busy || esp_timer_get_time() - s_last_start_us < MIN_GAP_US) return nullptr;
    return s_inbox;
}

void face_submit(void)
{
    s_busy = true;
    xTaskNotifyGive(s_task);
}
