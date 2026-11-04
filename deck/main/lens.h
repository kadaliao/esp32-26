#pragma once
// 摄像头（GC0308，DVP）：只在有应用要看时上电。
// 取帧和预处理放在核 0 的 cam 任务里，渲染线程随时拿最新一帧，两边互不等待。

#include <stdbool.h>
#include <stdint.h>

#define LENS_W  320
#define LENS_H  240
#define LENS_MW (LENS_W / 2)  // 亮度图、运动图是半分辨率，正好和光层一样大
#define LENS_MH (LENS_H / 2)

typedef enum { LENS_OFF, LENS_STARTING, LENS_ON, LENS_FAILED } lens_state_t;

typedef struct {
    const uint16_t *img;    // LENS_W × LENS_H，RGB565 小端（和 gfx_fb 同格式），已经是镜子方向
    const uint8_t *luma;    // LENS_MW × LENS_MH 亮度
    const uint8_t *motion;  // LENS_MW × LENS_MH 和上一帧的亮度差
    int moving;             // motion 超过 LENS_MOTION_T 的格子数
    uint32_t seq;           // 帧号：和上次一样就是同一帧
} lens_frame_t;

#define LENS_MOTION_T 24

void lens_init(void);    // 开机调一次：建任务，摄像头先不上电
void lens_start(void);   // 异步：上电、初始化、开始出帧（几百毫秒后才有第一帧）
void lens_stop(void);    // 异步：停下、断电
lens_state_t lens_state(void);
const char *lens_error(void);  // LENS_FAILED 时的原因
float lens_fps(void);

// 渲染线程用：拿最新一帧，没有帧时返回 false。拿到的这帧在下次调用前一直有效
bool lens_latest(lens_frame_t *out);
// 同上，给别的线程用：每个读者一个编号，各自拿着的那帧互不影响
enum { LENS_READER_RENDER = 0, LENS_READER_RELAY = 1 };
bool lens_latest_for(int reader, lens_frame_t *out);
