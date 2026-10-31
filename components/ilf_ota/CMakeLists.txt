# Обновление прошивки от барашка к барашку. Чистый C: радио и флеш приходят
# снаружи через ilf_ota_io_t, поэтому протокол проверяется тестами на ПК.
if(ESP_PLATFORM)
    idf_component_register(SRCS "ilf_ota.c" INCLUDE_DIRS "include")
else()
    add_library(ilf_ota STATIC ilf_ota.c)
    target_include_directories(ilf_ota PUBLIC include)
endif()
