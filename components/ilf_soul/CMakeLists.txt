# Душа и часы — чистый C, собираются и в прошивку, и в тесты/симулятор на ПК.
if(ESP_PLATFORM)
    idf_component_register(SRCS "ilf_soul.c" "ilf_clock.c" "ilf_lock.c"
                           INCLUDE_DIRS "include")
else()
    add_library(ilf_soul STATIC ilf_soul.c ilf_clock.c ilf_lock.c)
    target_include_directories(ilf_soul PUBLIC include)
endif()
