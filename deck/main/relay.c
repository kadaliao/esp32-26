#include "relay.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lens.h"
#include "link.h"

static const char *TAG = "relay";

#define MAX_INFLIGHT 2        // 最多几帧在路上：多一帧让 USB 传输和 Mac 推理重叠起来
#define INFLIGHT_MS  1500     // 这么久没收到回音就当丢了（Mac 没在跑、或者丢了帧），接着送
#define JPEG_Q       70       // 送去 Mac 的画面：反正要重画一遍，不用太清楚，编得快一些
#define JPG_MAX      (64 * 1024)

// 两个任务：
//   enc（核 0）：摄像头最新一帧 → JPEG → 发给 Mac
//   dec（核 1，优先级比渲染低）：Mac 画好的 JPEG → RGB565，只用渲染剩下的空闲时间
// 编码器、解码器各归一个任务管，只在 GHIBLI 开着时才打开（它们占不少内部 RAM）
static jpeg_enc_handle_t s_enc;
static jpeg_dec_handle_t s_dec;
static uint8_t *s_jpg;  // 编码输出

// 收：两块缓冲。串口线程写一块，dec 任务解另一块，互不踩
static uint8_t *s_in[2];
static size_t s_in_len[2];
static int s_in_ready = -1, s_decoding = -1;

// 解码好的画面：三块轮换，和 lens 一样
static uint16_t *s_out[3];
static int s_out_ready = -1, s_out_reading = -1;
static uint32_t s_out_seq;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;  // 护住上面这些下标，以及在路上的帧数
static TaskHandle_t s_enc_task, s_dec_task;
static volatile bool s_on;
static int s_inflight;
static int64_t s_sent_ms[MAX_INFLIGHT + 1];  // 在路上那几帧的发送时刻，先进先出
static relay_stats_t s_st;
static int s_got;          // 统计帧率用
static int64_t s_got_t0;

// 串口线程里调：拷进不在解码的那块缓冲，叫醒 dec 任务
static void on_fx(const uint8_t *data, size_t len)
{
    if (len > JPG_MAX) return;
    taskENTER_CRITICAL(&s_mux);
    int b = s_decoding == 0 ? 1 : 0;
    if (s_in_ready == b) s_in_ready = -1;  // 上一张还没来得及解：不要了，正在被覆盖
    // 一帧回来了：在路上的少一个，最早送出的那帧算延迟
    if (s_inflight > 0) {
        s_st.latency_ms = (float)(esp_timer_get_time() / 1000 - s_sent_ms[0]);
        memmove(s_sent_ms, s_sent_ms + 1, sizeof(s_sent_ms) - sizeof(s_sent_ms[0]));
        s_inflight--;
    }
    taskEXIT_CRITICAL(&s_mux);
    memcpy(s_in[b], data, len);
    s_in_len[b] = len;
    taskENTER_CRITICAL(&s_mux);
    s_in_ready = b;
    taskEXIT_CRITICAL(&s_mux);
    xTaskNotifyGive(s_dec_task);
    xTaskNotifyGive(s_enc_task);  // 腾出一个名额：可以再送一帧了
}

// ---------------------------------------------------------------- 解码（核 1）

static void dec_task(void *arg)
{
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        if (!s_on) {
            if (s_dec) {
                jpeg_dec_close(s_dec);
                s_dec = NULL;
            }
            continue;
        }
        if (!s_dec) {
            if (!s_enc) continue;  // 等编码器先开（它又在等摄像头）：内部 RAM 按这个顺序分，谁也不抢谁的
            size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            jpeg_dec_config_t dc = DEFAULT_JPEG_DEC_CONFIG();
            dc.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
            if (jpeg_dec_open(&dc, &s_dec) != JPEG_ERR_OK) {
                ESP_LOGE(TAG, "jpeg decoder open failed");
                s_dec = NULL;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            ESP_LOGW(TAG, "decoder open: %u bytes internal, %u left",
                     (unsigned)(before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }
        taskENTER_CRITICAL(&s_mux);
        int d = s_in_ready;
        s_in_ready = -1;
        s_decoding = d;
        int w = 0;
        while (w == s_out_ready || w == s_out_reading) w++;
        taskEXIT_CRITICAL(&s_mux);
        if (d < 0) continue;

        int64_t t0 = esp_timer_get_time();
        jpeg_dec_io_t io = {.inbuf = s_in[d], .inbuf_len = (int)s_in_len[d], .outbuf = (uint8_t *)s_out[w]};
        jpeg_dec_header_info_t info = {0};
        bool ok = jpeg_dec_parse_header(s_dec, &io, &info) == JPEG_ERR_OK && info.width == LENS_W &&
                  info.height == LENS_H && jpeg_dec_process(s_dec, &io) == JPEG_ERR_OK;
        int64_t t1 = esp_timer_get_time();
        taskENTER_CRITICAL(&s_mux);
        s_decoding = -1;
        if (ok) {
            s_out_ready = w;
            s_out_seq++;
        }
        taskEXIT_CRITICAL(&s_mux);
        if (!ok) {
            ESP_LOGW(TAG, "bad frame from host (%u bytes, %ux%u)", (unsigned)s_in_len[d], info.width, info.height);
            continue;
        }
        int64_t now_ms = t1 / 1000;
        s_st.last_ms = now_ms;
        s_st.dec_ms = (t1 - t0) / 1000.0f;
        s_got++;
        if (now_ms - s_got_t0 >= 2000) {
            s_st.fps = s_got * 1000.0f / (float)(now_ms - s_got_t0);
            s_got = 0;
            s_got_t0 = now_ms;
        }
    }
}

// ---------------------------------------------------------------- 编码、发送（核 0）

// 要等摄像头先起来再开：编码器占一块内部 RAM，抢在前面的话摄像头要的那块连续内存就没了
static bool enc_open(void)
{
    if (s_enc) return true;
    if (lens_state() != LENS_ON) return false;
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    jpeg_enc_config_t ec = DEFAULT_JPEG_ENC_CONFIG();
    ec.width = LENS_W;
    ec.height = LENS_H;
    ec.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    ec.subsampling = JPEG_SUBSAMPLE_420;
    ec.quality = JPEG_Q;  // 双任务模式（哈夫曼放到另一核）要在内部 RAM 开任务栈，这里挤不出来，用单任务
    static int64_t fail_ms;
    int64_t now = esp_timer_get_time() / 1000;
    if (fail_ms && now - fail_ms < 1000) return false;  // 刚失败过：一秒后再试，别刷屏
    if (jpeg_enc_open(&ec, &s_enc) != JPEG_ERR_OK) {
        if (!fail_ms) ESP_LOGE(TAG, "jpeg encoder open failed, internal free %u",
                               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        fail_ms = now;
        s_enc = NULL;
        return false;
    }
    fail_ms = 0;
    ESP_LOGW(TAG, "encoder open: %u bytes internal, %u left",
             (unsigned)(before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

static void send_one(int64_t now_ms)
{
    static uint32_t last_seq;
    lens_frame_t f;
    if (!lens_latest_for(LENS_READER_RELAY, &f) || f.seq == last_seq) return;
    last_seq = f.seq;
    int len = 0;
    int64_t t0 = esp_timer_get_time();
    jpeg_error_t err = jpeg_enc_process(s_enc, (const uint8_t *)f.img, LENS_W * LENS_H * 2, s_jpg, JPG_MAX, &len);
    int64_t t1 = esp_timer_get_time();
    if (err != JPEG_ERR_OK || len <= 0) {
        ESP_LOGW(TAG, "encode failed: %d", err);
        return;
    }
    if (!link_write_bin("cam", s_jpg, (size_t)len, 1000)) return;  // 头发出去了数据就得发完，不然主机会失步
    taskENTER_CRITICAL(&s_mux);
    s_sent_ms[s_inflight++] = now_ms;
    taskEXIT_CRITICAL(&s_mux);
    s_st.sent_kb = (len + 512) / 1024;
    s_st.enc_ms = (t1 - t0) / 1000.0f;
}

static void enc_task(void *arg)
{
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_on ? 8 : 500));
        if (!s_on) {
            if (s_enc) {
                jpeg_enc_close(s_enc);
                s_enc = NULL;
            }
            continue;
        }
        if (!enc_open()) continue;
        int64_t now_ms = esp_timer_get_time() / 1000;
        taskENTER_CRITICAL(&s_mux);
        if (s_inflight > 0 && now_ms - s_sent_ms[s_inflight - 1] > INFLIGHT_MS) {
            s_inflight = 0;  // 太久没回音：Mac 没在处理，从头来
        }
        bool room = s_inflight < MAX_INFLIGHT;
        taskEXIT_CRITICAL(&s_mux);
        if (room) send_one(now_ms);
    }
}


// ----------------------------------------------------------------

void relay_init(void)
{
    s_jpg = heap_caps_malloc(JPG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (int i = 0; i < 2; i++) s_in[i] = heap_caps_malloc(JPG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (int i = 0; i < 3; i++) s_out[i] = jpeg_calloc_align(LENS_W * LENS_H * 2, 16);  // 解码器要求 16 字节对齐
    if (!s_jpg || !s_in[0] || !s_in[1] || !s_out[0] || !s_out[1] || !s_out[2]) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }
    // 两个任务的栈都放 PSRAM：内部 RAM 留给摄像头 DMA 和编解码器（它们不碰 flash 写，放 PSRAM 没问题）
    if (xTaskCreatePinnedToCoreWithCaps(enc_task, "relay_enc", 4096, NULL, 3, &s_enc_task, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS ||
        xTaskCreatePinnedToCoreWithCaps(dec_task, "relay_dec", 4096, NULL, 1, &s_dec_task, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return;
    }
    link_on_bin("fx", on_fx);
}

void relay_start(void)
{
    if (!s_enc_task) return;
    taskENTER_CRITICAL(&s_mux);
    s_inflight = 0;
    s_out_ready = s_out_reading = -1;  // 上次留下的旧画面不要
    taskEXIT_CRITICAL(&s_mux);
    s_got = 0;
    s_got_t0 = esp_timer_get_time() / 1000;
    memset(&s_st, 0, sizeof(s_st));
    s_on = true;
    xTaskNotifyGive(s_enc_task);
    xTaskNotifyGive(s_dec_task);
}

void relay_stop(void)
{
    s_on = false;
    if (s_enc_task) {
        xTaskNotifyGive(s_enc_task);
        xTaskNotifyGive(s_dec_task);
    }
}

bool relay_active(void)
{
    return s_on;
}

bool relay_latest(const uint16_t **img, uint32_t *seq)
{
    taskENTER_CRITICAL(&s_mux);
    if (s_out_ready >= 0) s_out_reading = s_out_ready;
    int r = s_out_reading;
    uint32_t q = s_out_seq;
    taskEXIT_CRITICAL(&s_mux);
    if (r < 0) return false;
    *img = s_out[r];
    *seq = q;
    return true;
}

void relay_stats(relay_stats_t *out)
{
    *out = s_st;
}
