# Игры — чистый C, собираются и в прошивку, и в тесты/симулятор на ПК.
if(ESP_PLATFORM)
    idf_component_register(SRCS "ilf_games.c"
                           INCLUDE_DIRS "include")
else()
    add_library(ilf_games STATIC ilf_games.c)
    target_include_directories(ilf_games PUBLIC include)
    target_link_libraries(ilf_games PUBLIC m)
endif()
