set(ILF_UI_SRCS
    ilf_ui.c
    ilf_eye_img.c
    fonts/ilf_font_24.c
    fonts/ilf_font_32.c
    fonts/ilf_font_48.c
    fonts/ilf_font_digits_120.c)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${ILF_UI_SRCS}
                           INCLUDE_DIRS "include"
                           REQUIRES lvgl ilf_soul)
else()
    add_library(ilf_ui STATIC ${ILF_UI_SRCS})
    target_include_directories(ilf_ui PUBLIC include)
    target_link_libraries(ilf_ui PUBLIC lvgl ilf_soul)
endif()
