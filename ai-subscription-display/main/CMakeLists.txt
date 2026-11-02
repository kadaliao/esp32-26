idf_component_register(
    SRCS "main.c" "board.c" "link.c" "ui.c" "shot.c" "logos.c"
         "fonts/lv_font_yen_10.c" "fonts/lv_font_yen_14.c" "fonts/lv_font_yen_16.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_usb_serial_jtag esp_driver_spi esp_driver_i2c esp_driver_ledc
             esp_lcd cjson esp_timer
    PRIV_REQUIRES driver
)
