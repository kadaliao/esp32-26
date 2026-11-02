#pragma once

#include "lvgl.h"

// 由 lv_font_conv 从 LVGL 自带的 Montserrat-Medium.ttf 生成，
// 范围 0x20-0x7E + ¥(0xA5) + ·(0xB7) + —(0x2014)。
// 内置的 lv_font_montserrat_* 不含 ¥，余额会显示成方块，所以单独生成一套。
LV_FONT_DECLARE(lv_font_yen_10);
LV_FONT_DECLARE(lv_font_yen_14);
LV_FONT_DECLARE(lv_font_yen_16);
