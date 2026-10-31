idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES ilf_hal ilf_soul ilf_ui nvs_flash esp_timer)
