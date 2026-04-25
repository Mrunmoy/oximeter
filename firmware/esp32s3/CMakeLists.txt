# PHASE 2 — skeleton; first prototype runs on RP2040
#
# Top-level CMakeLists for the ESP32-S3-Zero firmware target. Mirrors the
# esp-clock layout: minimal-build property, C++17, ESP-IDF v5.5 project
# template. The portable driver core under lib/max3010x/ is pulled into
# main/ via reach-across SRCS in main/CMakeLists.txt.

cmake_minimum_required(VERSION 3.16)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)

project(oxinode)
