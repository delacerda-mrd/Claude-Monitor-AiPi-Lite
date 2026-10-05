cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "2.5.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(claude_meter_v2)

# LVGL allocates from PSRAM (main/lvgl_port/lv_mem_psram.h): its objects were
# landing in scarce internal RAM (small mallocs stay internal, ALWAYSINTERNAL).
# Draw buffers stay static in internal RAM (main.c).
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_include_directories(${lvgl_lib} PRIVATE ${CMAKE_SOURCE_DIR}/main/lvgl_port)
target_compile_definitions(${lvgl_lib} PRIVATE
    LV_MEM_CUSTOM_ALLOC=lv_psram_alloc LV_MEM_CUSTOM_FREE=lv_psram_free LV_MEM_CUSTOM_REALLOC=lv_psram_realloc)
