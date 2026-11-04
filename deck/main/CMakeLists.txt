idf_component_register(
    SRCS "main.c" "board.c" "link.c" "shot.c" "gfx.c" "fonts.c" "logo.c" "icons.c"
         "input.c" "shell.c" "lens.c" "face.cpp" "relay.c"
         "apps/sys.c" "apps/magpie.c" "apps/pond.c" "apps/quota.c" "apps/logos.c" "apps/mirror.c" "apps/eyes.c"
    INCLUDE_DIRS "." "apps"
    EMBED_FILES "cjk12.bin"
    REQUIRES esp_driver_usb_serial_jtag esp_driver_spi esp_driver_i2c esp_driver_ledc esp_driver_gpio
             esp_lcd cjson esp_timer nvs_flash esp32-camera human_face_detect esp_new_jpeg
)

# 往固定长度的显示缓冲里截断写字符串是有意的
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
# sqrtf 之类不设 errno：编译器才会用 FPU 指令内联，而不是去调库函数（画眼睛、光晕每个像素都要用）
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-math-errno)
