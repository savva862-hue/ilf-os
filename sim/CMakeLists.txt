cmake_minimum_required(VERSION 3.16)
project(ilf_sim C)
set(CMAKE_C_STANDARD 11)

set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE STRING "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES 1 CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS 1 CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL 1 CACHE BOOL "" FORCE)
add_subdirectory(../components/lvgl lvgl)

add_subdirectory(../components/ilf_soul ilf_soul)
add_subdirectory(../components/ilf_herd ilf_herd)
add_subdirectory(../components/ilf_games ilf_games)
add_subdirectory(../components/ilf_ui ilf_ui)

add_executable(ilf_sim sim_main.c)
target_link_libraries(ilf_sim ilf_ui ilf_soul lvgl m)
