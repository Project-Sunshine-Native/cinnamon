cmake_minimum_required(VERSION 3.21)
project(cinnamon C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

set(PLATFORM "" CACHE STRING "Platform backend")

# Build metadata
set(cinnamon_COMMIT_HASH "unknown" CACHE STRING "Git commit hash")
set(cinnamon_COMMIT_DATE "unknown" CACHE STRING "Git commit date")
add_compile_definitions(
    cinnamon_COMMIT_HASH="${cinnamon_COMMIT_HASH}"
    cinnamon_COMMIT_DATE="${cinnamon_COMMIT_DATE}"
)

if(PLATFORM STREQUAL "")
    message(FATAL_ERROR "PLATFORM is not set! Use -DPLATFORM=n3ds -DN3DS=1 -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake (or another supported platform).")
endif()

file(GLOB SOURCES src/*.c)
# Platform specific files builds
file(GLOB PLATFORM_SOURCES src/${PLATFORM}/*.c)

add_executable(cinnamon ${SOURCES} ${PLATFORM_SOURCES})
target_include_directories(cinnamon PRIVATE ${CMAKE_SOURCE_DIR}/src)

# stb_ds
target_include_directories(cinnamon PUBLIC vendor/stb/ds)

if(PLATFORM STREQUAL "glfw")
    # GLAD
    add_library(glad STATIC vendor/glad/src/glad.c)
    target_include_directories(glad PUBLIC vendor/glad/include)

    # stb_image
    target_include_directories(cinnamon PUBLIC vendor/stb/image)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLFW3 REQUIRED glfw3)
    target_include_directories(cinnamon PRIVATE ${GLFW3_INCLUDE_DIRS})
    target_link_libraries(cinnamon ${GLFW3_LIBRARIES} glad m)

    # Enable useful things to figure out when things have gone wrong related to memory corruption
    # (Also set -DCMAKE_BUILD_TYPE=Debug to get debug symbols!)
    set(ENABLE_ASAN OFF CACHE BOOL "Enable AddressSanitizer")
    if(ENABLE_ASAN)
        target_compile_options(cinnamon PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(cinnamon PRIVATE -fsanitize=address)
    endif()
elseif(PLATFORM STREQUAL "ps2")
    if(PS2)
        # Compiling on the Docker container
        target_include_directories(cinnamon PRIVATE $ENV{PS2SDK}/ee/include $ENV{PS2SDK}/common/include $ENV{PS2DEV}/ee/include $ENV{GSKIT}/include)
        target_link_directories(cinnamon PRIVATE $ENV{PS2SDK}/ee/lib $ENV{GSKIT}/lib)
        target_link_libraries(cinnamon gskit dmakit pad mc patches kernel c m)

        # Embed ps2sdk IRX modules (sio2man, padman) as C arrays via bin2c
        # This avoids loading from rom0: which has compatibility issues between fat/slim PS2
        set(IRX_DIR $ENV{PS2SDK}/iop/irx)
        set(IRX_GEN_DIR ${CMAKE_BINARY_DIR}/generated/irx)
        file(MAKE_DIRECTORY ${IRX_GEN_DIR})

        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/sio2man_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/sio2man.irx ${IRX_GEN_DIR}/sio2man_irx.c sio2man_irx
            DEPENDS ${IRX_DIR}/sio2man.irx
            COMMENT "Embedding sio2man.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/mcman_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/mcman.irx ${IRX_GEN_DIR}/mcman_irx.c mcman_irx
            DEPENDS ${IRX_DIR}/mcman.irx
            COMMENT "Embedding mcman.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/mcserv_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/mcserv.irx ${IRX_GEN_DIR}/mcserv_irx.c mcserv_irx
            DEPENDS ${IRX_DIR}/mcserv.irx
            COMMENT "Embedding mcserv.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/padman_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/padman.irx ${IRX_GEN_DIR}/padman_irx.c padman_irx
            DEPENDS ${IRX_DIR}/padman.irx
            COMMENT "Embedding padman.irx"
        )

        target_sources(cinnamon PRIVATE ${IRX_GEN_DIR}/sio2man_irx.c ${IRX_GEN_DIR}/mcman_irx.c ${IRX_GEN_DIR}/mcserv_irx.c ${IRX_GEN_DIR}/padman_irx.c)
        target_include_directories(cinnamon PRIVATE ${IRX_GEN_DIR})
    else()
        # This block is for when we are developing cinnamon with CLion!
        # ps2sdk and gsKit scatter headers across per-module subdirectories;
        # the flat include/ layout only exists after building the SDK.
        # We use file(GLOB) to discover all include dirs automatically.
        file(GLOB PS2SDK_EE_INCLUDES ${CMAKE_SOURCE_DIR}/ps2sdk/ee/*/include)
        file(GLOB PS2SDK_EE_RPC_INCLUDES ${CMAKE_SOURCE_DIR}/ps2sdk/ee/rpc/*/include)
        file(GLOB PS2SDK_EE_NETWORK_INCLUDES ${CMAKE_SOURCE_DIR}/ps2sdk/ee/network/*/include)
        file(GLOB GSKIT_INCLUDES ${CMAKE_SOURCE_DIR}/gsKit/ee/*/include)

        # _EE is normally set by the PS2 toolchain; without it, tamtypes.h
        # refuses to define u64/u32/etc. We set it so CLion can resolve them.
        target_compile_definitions(cinnamon PRIVATE _EE)

        target_include_directories(cinnamon PRIVATE
                ${PS2SDK_EE_INCLUDES}
                ${PS2SDK_EE_RPC_INCLUDES}
                ${PS2SDK_EE_NETWORK_INCLUDES}
                ${CMAKE_SOURCE_DIR}/ps2sdk/common/include
                ${GSKIT_INCLUDES}
                # Newlib and GCC built-in headers are build artifacts (not in any repo),
                # so these are still extracted from Docker by setup.sh.
                ${CMAKE_SOURCE_DIR}/ps2dev-headers/newlib-include
                ${CMAKE_SOURCE_DIR}/ps2dev-headers/gcc-include
        )
    endif()
elseif(PLATFORM STREQUAL "n3ds")
    if(NOT DEFINED ENV{DEVKITPRO})
        message(FATAL_ERROR "DEVKITPRO environment variable is not set! Install devkitPro and set DEVKITPRO.")
    endif()

    set(DEVKITPRO $ENV{DEVKITPRO})
    set(CTRULIB   ${DEVKITPRO}/libctru)
    set(PORTLIBS  ${DEVKITPRO}/portlibs/3ds)

    # ARM11 MPCore flags matching the 3DS hardware (mirrors Makefile ARCH / CFLAGS)
    target_compile_options(cinnamon PRIVATE
        -g -O2 -mword-relocations -ffunction-sections
        -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
    )
    target_compile_definitions(cinnamon PRIVATE __3DS__)

    target_include_directories(cinnamon PRIVATE
        ${CTRULIB}/include
        ${PORTLIBS}/include
    )

    # stb_image
    target_include_directories(cinnamon PUBLIC vendor/stb/image)
    
    target_link_directories(cinnamon PRIVATE
        ${CTRULIB}/lib
        ${PORTLIBS}/lib
    )

    target_link_libraries(cinnamon citro2d citro3d ctru m)

    if(N3DS)
        # ── Post-build: generate SMDH banner then the final .3dsx ──────────────

        # App metadata (mirrors APP_TITLE / APP_DESCRIPTION / APP_AUTHOR in Makefile)
        set(N3DS_APP_TITLE       "Cinnamon" CACHE STRING "3DS SMDH title")
        set(N3DS_APP_DESCRIPTION "A GameMaker runner for the N3DS"         CACHE STRING "3DS SMDH description")
        set(N3DS_APP_AUTHOR      "Owen-Programming"         CACHE STRING "3DS SMDH author")

        # Fall back to devkitPro's built-in icon when none is supplied
        if(NOT DEFINED N3DS_APP_ICON OR N3DS_APP_ICON STREQUAL "")
            set(N3DS_APP_ICON "${CTRULIB}/default_icon.png")
        endif()

        set(SMDH_OUTPUT "${CMAKE_BINARY_DIR}/cinnamon.smdh")
        set(TDSX_OUTPUT "${CMAKE_BINARY_DIR}/cinnamon.3dsx")

        add_custom_command(
            TARGET cinnamon POST_BUILD
            COMMAND ${DEVKITPRO}/tools/bin/smdhtool
                --create
                "${N3DS_APP_TITLE}"
                "${N3DS_APP_DESCRIPTION}"
                "${N3DS_APP_AUTHOR}"
                "${N3DS_APP_ICON}"
                "${SMDH_OUTPUT}"
            COMMENT "Generating cinnamon.smdh"
        )

        add_custom_command(
            TARGET cinnamon POST_BUILD
            COMMAND ${DEVKITPRO}/tools/bin/3dsxtool
                $<TARGET_FILE:cinnamon>
                "${TDSX_OUTPUT}"
                "--smdh=${SMDH_OUTPUT}"
            COMMENT "Generating cinnamon.3dsx"
        )
    else()
        # ── CLion / host development (no devkitARM toolchain active) ───────────
        # Glob all devkitPro include trees so the IDE can resolve 3DS headers.
        # __3DS__ is already defined above so the headers open their guarded sections.
        file(GLOB CTRULIB_INCLUDES  ${CTRULIB}/include)
        file(GLOB PORTLIBS_INCLUDES ${PORTLIBS}/include)

        target_include_directories(cinnamon PRIVATE
            ${CTRULIB_INCLUDES}
            ${PORTLIBS_INCLUDES}
        )
    endif()

else()
    message(FATAL_ERROR "Unknown platform! ${PLATFORM}")
endif()