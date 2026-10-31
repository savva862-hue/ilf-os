# Стадо: знакомство, друзья, передача голосовых и рисунков, сжатие голоса.
# Чистый C: собирается и в прошивку, и в тесты на ПК. Радио и криптография
# приходят снаружи через ilf_herd_io_t.
if(ESP_PLATFORM)
    idf_component_register(SRCS "ilf_herd.c" "ilf_adpcm.c"
                           INCLUDE_DIRS "include")
else()
    add_library(ilf_herd STATIC ilf_herd.c ilf_adpcm.c)
    target_include_directories(ilf_herd PUBLIC include)
endif()
