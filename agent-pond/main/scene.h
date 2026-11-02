#pragma once

#include <stdint.h>
#include "model.h"

// 夜池场景：每个 agent 是一只萤火虫
void scene_init(void);
void scene_update(const snapshot_t *snap);       // 主机来了新数据
void scene_frame(float dt, int64_t now_ms);       // 推进一帧并画到 gfx_fb
