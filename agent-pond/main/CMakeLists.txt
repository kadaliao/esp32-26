idf_component_register(
    SRCS "main.c" "board.c" "link.c" "shot.c" "gfx.c" "scene.c" "fonts.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_usb_serial_jtag esp_driver_spi esp_driver_i2c esp_driver_ledc
             esp_lcd cjson esp_timer
)
