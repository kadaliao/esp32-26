#pragma once

#include <stdint.h>
#include "model.h"

// 实时网关：左边 agent，右边 provider，中间是 magpie；每个请求是一只喜鹊
void scene_init(void);
void scene_update(const snapshot_t *snap);       // 主机来了新数据
void scene_frame(float dt, int64_t now_ms);       // 推进一帧并画到 gfx_fb
