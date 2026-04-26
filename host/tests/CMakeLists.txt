cmake_minimum_required(VERSION 3.16)
project(oxinode_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Pull in the portable driver cores. Paths are relative to this
# CMakeLists so the files are movable; binary dirs are namespaced
# under the parent build tree to avoid stomping when a firmware build
# also includes them.
add_subdirectory(../../lib/max3010x ${CMAKE_BINARY_DIR}/lib_max3010x)
add_subdirectory(../../lib/ssd1306  ${CMAKE_BINARY_DIR}/lib_ssd1306)

# GoogleTest: prefer a system install; fall back to FetchContent so
# `cmake -B build/host` works on a bare machine without manual apt-get.
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    # Don't install gtest's headers — we only use it for tests here.
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK   ON  CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

enable_testing()
include(GoogleTest)

add_executable(oxinode_tests
    test_max30102.cpp
    test_hr_detector.cpp
    test_spo2_algo.cpp
    test_framer.cpp
    test_ssd1306.cpp
)

target_include_directories(oxinode_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
)

# Test code may use STL freely; matches lib's strictness on warnings
# but keeps exceptions/RTTI on (gtest needs them).
target_compile_options(oxinode_tests PRIVATE
    -Wall -Wextra -Werror
)

if(TARGET GTest::gtest_main)
    target_link_libraries(oxinode_tests PRIVATE
        oxinode_max3010x
        oxinode_ssd1306
        GTest::gtest_main
    )
else()
    target_link_libraries(oxinode_tests PRIVATE
        oxinode_max3010x
        oxinode_ssd1306
        gtest_main
    )
endif()

gtest_discover_tests(oxinode_tests)
