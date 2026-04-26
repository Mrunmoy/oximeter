cmake_minimum_required(VERSION 3.13)

# ── Pico SDK bootstrap ───────────────────────────────────────────
# PICO_SDK_PATH is exported by the Nix devShell. Including the SDK's
# pico_sdk_init.cmake must happen before project() because it injects
# the cross-compile toolchain file for arm-none-eabi.
include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(oxinode_rp2040 C CXX ASM)

set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generic RP2040 board (the Waveshare RP2040-Zero is a vanilla RP2040;
# pin overrides live in include/board/pins.hpp).
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico CACHE STRING "Pico SDK board header")
endif()

pico_sdk_init()

# ── Global C++ ABI flags ─────────────────────────────────────────
# These affect ABI / linkage and must match the SDK's own build:
# apply globally. Strict warning flags are NOT applied globally
# because they would also cover the pico-sdk and tinyusb sources.
add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
)

# ── Build version ────────────────────────────────────────────────
# Resolved from `git describe` if available (tag + dirty marker), else
# falls back to "dev". Surfaced to firmware as OXINODE_BUILD_VERSION.
find_package(Git QUIET)
set(OXINODE_BUILD_VERSION "dev")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --always
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE _oxinode_git_describe
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _oxinode_git_rc
    )
    if(_oxinode_git_rc EQUAL 0 AND NOT "${_oxinode_git_describe}" STREQUAL "")
        set(OXINODE_BUILD_VERSION "${_oxinode_git_describe}")
    endif()
endif()
message(STATUS "OxiNode build version: ${OXINODE_BUILD_VERSION}")

# ── Helpers ──────────────────────────────────────────────────────
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/oxinode_app.cmake)

# ── Portable driver core ─────────────────────────────────────────
# Built as a static library targeting arm-none-eabi for this firmware.
# The driver core lives outside this directory tree on purpose: the
# same sources also build for x86 host tests under host/tests/.
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/max3010x
    ${CMAKE_BINARY_DIR}/lib_max3010x
)

# Portable SSD1306 OLED driver — same portability contract as
# lib/max3010x: only depends on its own ssd1306::IBus interface,
# no platform headers, host-testable.
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/ssd1306
    ${CMAKE_BINARY_DIR}/lib_ssd1306
)

# ── Applications ─────────────────────────────────────────────────
add_subdirectory(apps/oxinode)
