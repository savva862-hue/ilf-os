idf_component_register(SRCS "main.c" "ilf_net.c" "ilf_voice.c" "ilf_ble.c" "ilf_cloud.c"
                       INCLUDE_DIRS "."
                       REQUIRES ilf_hal ilf_soul ilf_ui ilf_herd ilf_phone ilf_ota bt app_update bootloader_support esp_partition nvs_flash esp_timer esp_wifi esp_event mbedtls spiffs fatfs esp_http_client esp_netif json ilf_games)
