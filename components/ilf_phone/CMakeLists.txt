# Разбор протоколов Apple (ANCS, AMS, CTS): чистый C, тестируется на ПК.
if(ESP_PLATFORM)
    idf_component_register(SRCS "ilf_phone.c" INCLUDE_DIRS "include")
else()
    add_library(ilf_phone STATIC ilf_phone.c)
    target_include_directories(ilf_phone PUBLIC include)
endif()
