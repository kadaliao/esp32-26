#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_lcd_types.h"

#define LCD_H_RES 320
#define LCD_V_RES 240

// 立创·实战派 ESP32-S3 板级初始化：I2C -> PCA9557 -> SPI -> ST7789 -> 背光
// 参数取自小智 lichuang-dev 板型（在这块板上实测可用）
esp_lcd_panel_handle_t board_init(void);

// I2C1 总线（触摸芯片和 IO 扩展挂在同一条上）
i2c_master_bus_handle_t board_i2c(void);

// 摄像头供电（PCA9557 bit2，低电平上电）。开机时是断电的
void board_camera_power(bool on);

// 背光亮度 0..100
void board_backlight(uint8_t percent);

// 阻塞等待上一次 SPI 刷屏完成（缓冲可复用）
void board_wait_flush(void);
