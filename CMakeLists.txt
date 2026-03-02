cmake_minimum_required(VERSION 3.21)
project(butterscotch C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

set(PLATFORM "" CACHE STRING "Platform backend")

if(PLATFORM STREQUAL "")
    message(FATAL_ERROR "PLATFORM is not set! Use -DPLATFORM=glfw (or another supported platform).")
endif()

file(GLOB SOURCES src/*.c)
add_executable(butterscotch ${SOURCES})
target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(PLATFORM STREQUAL "glfw")
    # GLAD
    add_library(glad STATIC vendor/glad/src/glad.c)
    target_include_directories(glad PUBLIC vendor/glad/include)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLFW3 REQUIRED glfw3)
    target_include_directories(butterscotch PRIVATE ${GLFW3_INCLUDE_DIRS})
    target_link_libraries(butterscotch ${GLFW3_LIBRARIES} glad)

    # Enable useful things to figure out when things have gone wrong related to memory corruption
    # (Also set -DCMAKE_BUILD_TYPE=Debug to get debug symbols!)
    target_compile_options(butterscotch PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(butterscotch PRIVATE -fsanitize=address)
else()
    message(FATAL_ERROR "Unknown platform! ${PLATFORM}")
endif()
