idf_component_register(SRCS "ilf_board.c" "ilf_display.c" "ilf_touch.c" "ilf_sensors.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_lcd esp_timer lvgl)
