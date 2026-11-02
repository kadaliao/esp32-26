#pragma once
#include <stdbool.h>
#include "logo.h"

// 主机发来的 agent / provider 图标缓存（18x18 RGB565 + alpha）
#define ICON_PX 18

void icons_put(const char *key, const char *hex);   // hex: 每像素 6 个十六进制字符（565 小端 + alpha）
const image_t *icons_get(const char *key);           // 没有就返回 NULL
