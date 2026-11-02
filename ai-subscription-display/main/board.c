#include "board.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "board";

// ---- 实战派 ESP32-S3 引脚（来源：小智 lichuang-dev 板型，已在此板验证）----
#define PIN_I2C_SDA      1
#define PIN_I2C_SCL      2
#define PIN_LCD_MOSI     40
#define PIN_LCD_SCLK     41
#define PIN_LCD_DC       39
#define PIN_LCD_BL       42   // 反相：低电平点亮

#define PCA9557_ADDR     0x19
#define PCA9557_REG_OUT  0x01
#define PCA9557_REG_CFG  0x03

static i2c_master_dev_handle_t s_pca = NULL;
static SemaphoreHandle_t s_flush_done = NULL;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata,
                                void *ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &hp);
    return hp == pdTRUE;
}

void board_wait_flush(void)
{
    if (s_flush_done) {
        xSemaphoreTake(s_flush_done, portMAX_DELAY);
    }
}

static esp_err_t pca_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_pca, buf, sizeof(buf), 1000);
}

static esp_err_t pca_read(uint8_t reg, uint8_t *out)
{
    return i2c_master_transmit_receive(s_pca, &reg, 1, out, 1, 1000);
}

// bit0: 屏幕供电（低有效）  bit1: 功放使能  bit2: 摄像头供电（低有效）
static esp_err_t pca_set_bit(uint8_t bit, uint8_t level)
{
    uint8_t cur = 0;
    ESP_RETURN_ON_ERROR(pca_read(PCA9557_REG_OUT, &cur), TAG, "pca read");
    cur = (cur & ~(1 << bit)) | (level << bit);
    return pca_write(PCA9557_REG_OUT, cur);
}

static esp_err_t i2c_and_expander_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = 1,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &bus), TAG, "i2c bus");

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCA9557_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev, &s_pca), TAG, "pca add");

    // 与官方固件一致：bit0/1/2 为输出，其余为输入；初始 bit0=1(断电) bit1=1 bit2=0
    ESP_RETURN_ON_ERROR(pca_write(PCA9557_REG_CFG, 0xF8), TAG, "pca cfg");
    ESP_RETURN_ON_ERROR(pca_write(PCA9557_REG_OUT, 0x03), TAG, "pca out");
    return ESP_OK;
}

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&t), TAG, "ledc timer");

    ledc_channel_config_t c = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_1,
        .duty = 1023, // 反相：满占空比 = 最暗
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&c), TAG, "ledc chan");
    return ESP_OK;
}

void board_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    // 反相：亮度 100% -> duty 0
    uint32_t duty = (uint32_t)((100 - percent) * 1023 / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_lcd_panel_handle_t board_init(void)
{
    ESP_ERROR_CHECK(i2c_and_expander_init());
    ESP_ERROR_CHECK(backlight_init());

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = -1,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 2,
        .pclk_hz = 80 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_cfg, &io));

    s_flush_done = xSemaphoreCreateBinary();
    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &cbs, NULL));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1, // 硬件复位走 PCA9557
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    // 官方次序：面板复位后由 IO 扩展器给屏幕上电
    ESP_ERROR_CHECK(pca_set_bit(0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    board_backlight(100);
    ESP_LOGI(TAG, "ST7789 %dx%d ready", LCD_H_RES, LCD_V_RES);
    return panel;
}
