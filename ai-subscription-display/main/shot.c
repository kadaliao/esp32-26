#include "shot.h"

#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "link.h"
#include "ui.h"

static const char *TAG = "shot";

// 行编码（每行把像素自身的 RGB565 值写入文件，主机据此还原）：
//   tag 最高位 1 -> 重复游程：run = (tag & 0x7F) + 1，随后 2 字节像素值
//   tag 最高位 0 -> 字面量块：n   = (tag & 0x7F) + 1，随后 n*2 字节原样像素
#define RUN_MAX 128

static size_t rle_encode(const uint16_t *px, size_t n, uint8_t *out, size_t cap)
{
    size_t o = 0;
    size_t i = 0;
    while (i < n) {
        // 统计相同像素的连续长度
        size_t run = 1;
        while (i + run < n && px[i + run] == px[i] && run < RUN_MAX) {
            run++;
        }
        if (run >= 3) {
            if (o + 3 > cap) {
                return 0;
            }
            out[o++] = (uint8_t)(0x80 | (run - 1));
            out[o++] = (uint8_t)(px[i] & 0xFF);
            out[o++] = (uint8_t)(px[i] >> 8);
            i += run;
            continue;
        }
        // 字面量：一直收，直到遇到长度 >= 3 的游程或达到上限
        size_t lit = 0;
        while (i + lit < n && lit < RUN_MAX) {
            size_t r = 1;
            while (i + lit + r < n && px[i + lit + r] == px[i + lit] && r < 3) {
                r++;
            }
            if (r >= 3) {
                break;
            }
            lit++;
        }
        if (lit == 0) {
            lit = 1;
        }
        if (o + 1 + lit * 2 > cap) {
            return 0;
        }
        out[o++] = (uint8_t)(lit - 1);
        for (size_t k = 0; k < lit; k++) {
            out[o++] = (uint8_t)(px[i + k] & 0xFF);
            out[o++] = (uint8_t)(px[i + k] >> 8);
        }
        i += lit;
    }
    return o;
}

void shot_send(void)
{
    int w = 0;
    int h = 0;
    const uint16_t *px = ui_shadow(&w, &h);
    if (!px || w <= 0 || h <= 0) {
        link_printf("!!SHOTERR\n");
        return;
    }

    size_t npix = (size_t)w * (size_t)h;
    size_t cap = npix * 2 + 1024;
    uint8_t *rle = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rle) {
        rle = heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!rle) {
        link_printf("!!SHOTERR\n");
        return;
    }

    size_t len = rle_encode(px, npix, rle, cap);
    if (len == 0) {
        heap_caps_free(rle);
        link_printf("!!SHOTERR\n");
        return;
    }

    link_printf("!!SHOT %d %d %u\n", w, h, (unsigned)len);

    static const char hexd[] = "0123456789abcdef";
    char line[1024];
    size_t i = 0;
    while (i < len) {
        size_t chunk = len - i;
        if (chunk > 400) {
            chunk = 400;
        }
        line[0] = '!';
        line[1] = '!';
        line[2] = 'D';
        line[3] = 'A';
        line[4] = 'T';
        line[5] = 'A';
        line[6] = ' ';
        size_t o = 7;
        for (size_t k = 0; k < chunk; k++) {
            uint8_t b = rle[i + k];
            line[o++] = hexd[b >> 4];
            line[o++] = hexd[b & 0x0F];
        }
        line[o++] = '\n';
        link_write(line, o, 3000);
        i += chunk;
    }
    link_write("!!SHOTEND\n", 10, 3000);
    heap_caps_free(rle);
    ESP_LOGI(TAG, "shot %dx%d rle=%u", w, h, (unsigned)len);
}
