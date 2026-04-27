cmake_minimum_required (VERSION 3.14)

project(
    Cinnamon
    VERSION 1.2
    LANGUAGES C
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

set(COMMON_SOURCES
    src/audio_system.h
    src/binary_reader.c
    src/binary_reader.h
    src/binary_utils.h
    src/collision.h
    src/data_win.c
    src/data_win.h
    src/data_win_print.c
    src/file_system.h
    src/ini.c
    src/ini.h
    src/input_recording.c
    src/input_recording.h
    src/instance.c
    src/instance.h
    src/json_reader.c
    src/json_reader.h
    src/json_writer.c
    src/json_writer.h
    src/matrix_math.h
    src/profiler.c
    src/profiler.h
    src/renderer.h
    src/runner.c
    src/runner.h
    src/runner_keyboard.c
    src/runner_keyboard.h
    src/rvalue.h
    src/text_utils.h
    src/utils.h
    src/vm.c
    src/vm.h
    src/vm_builtins.c
    src/vm_builtins.h
)

set(WIIU_SOURCES
    src/wiiu/wiiu_audio_system.c
    src/wiiu/wiiu_renderer.c
    src/wiiu/main.c
    src/wiiu/textured_quad_gsh.h
    src/wiiu/wiiu_file_system.c
    src/wiiu/wiiu_audio_system.h
    src/wiiu/pos_col_gsh.h
    src/wiiu/wiiu_renderer.h
    src/wiiu/stb_impl.c
    src/wiiu/wiiu_file_system.h
)

include_directories(vendor)

find_package(SDL2 REQUIRED)

add_executable(
    Cinnamon 
    ${COMMON_SOURCES} 
    ${WIIU_SOURCES}
)

target_link_libraries(Cinnamon PRIVATE SDL2::SDL2)

wut_create_rpx(Cinnamon)
wut_create_wuhb(Cinnamon)