cmake_minimum_required(VERSION 3.16)

# Standalone-build guard: if this directory is configured directly (not via
# add_subdirectory from a parent firmware project), provide a project name so
# CMake doesn't complain. When pulled in from firmware/rp2040 or host/tests
# the parent already declared a project.
if(NOT DEFINED PROJECT_NAME)
    project(oxinode_max3010x CXX)
endif()

add_library(oxinode_max3010x STATIC
    src/Max30102.cpp
    src/HrDetector.cpp
    src/Spo2Algo.cpp
    src/Framer.cpp
)

target_include_directories(oxinode_max3010x PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(oxinode_max3010x PUBLIC cxx_std_17)

set_target_properties(oxinode_max3010x PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# Strict warnings + no exceptions / RTTI: the driver-core contract is
# embedded-friendly. Tests link against this same library, so the flags
# follow through to host builds too.
target_compile_options(oxinode_max3010x PRIVATE
    -Wall -Wextra -Werror
    -fno-exceptions -fno-rtti
)

# PUBLIC propagation of -fno-exceptions/-fno-rtti would force the same on
# any consumer, including the gtest binary which needs exceptions. Keep
# them PRIVATE so consumers (firmware OR host tests) choose their own.
