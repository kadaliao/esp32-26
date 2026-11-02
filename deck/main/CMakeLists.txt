idf_component_register(
    SRCS "main.c" "board.c" "link.c" "shot.c" "gfx.c" "fonts.c" "logo.c" "icons.c"
         "input.c" "shell.c"
         "apps/sys.c" "apps/magpie.c" "apps/pond.c" "apps/quota.c" "apps/logos.c"
    INCLUDE_DIRS "." "apps"
    EMBED_FILES "cjk12.bin"
    REQUIRES esp_driver_usb_serial_jtag esp_driver_spi esp_driver_i2c esp_driver_ledc esp_driver_gpio
             esp_lcd cjson esp_timer nvs_flash
)

# 往固定长度的显示缓冲里截断写字符串是有意的
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
