#pragma once
// 人脸检测（乐鑫 ESP-DL 的 human_face_detect，MSR + MNP 两级模型）。
// 跑在核 0 的低优先级任务里，只吃核 0 的空闲时间；一次约 45ms，所以每秒能出十来个结果。
// 喂图的是 lens.c：检测器空闲时，它在处理每一帧的同一趟里顺手写一张半分辨率彩色图进收件箱。

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_W 160  // 收件箱尺寸：正好是模型第一级的输入，不用再缩放
#define FACE_H 120

typedef struct {
    bool found;
    int n;                 // 这一帧找到几张脸
    float x, y, w, h;      // 最大那张脸的框，摄像头画面坐标（320×240，镜子方向）
    float score;
    float ms;              // 这次检测花了多久
    int64_t at_ms;         // 检测完成的时刻
    uint32_t seq;          // 每出一个结果加一
} face_t;

void face_init(void);          // 开机调一次：建任务（模型等第一次要用时才加载）
void face_enable(bool on);     // 要不要检测（不管摄像头开关，摄像头还得 lens_start）
bool face_latest(face_t *out); // 最近一次结果；还没有结果时返回 false

// 给 lens.c 用：检测器空闲时返回收件箱（FACE_W × FACE_H，RGB565 小端），忙或没开时返回 NULL
uint16_t *face_inbox(void);
void face_submit(void);        // 收件箱写好了，开始检测

#ifdef __cplusplus
}
#endif
