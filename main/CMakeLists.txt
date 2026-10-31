idf_component_register(SRCS "main.c" "ilf_net.c" "ilf_voice.c"
                       INCLUDE_DIRS "."
                       REQUIRES ilf_hal ilf_soul ilf_ui ilf_herd nvs_flash esp_timer esp_wifi esp_event mbedtls spiffs)
