cmake_minimum_required(VERSION 3.16)

# Standalone-build guard: when included via add_subdirectory the parent
# already declared a project; this only fires on direct configure.
if(NOT DEFINED PROJECT_NAME)
    project(oxinode_ssd1306 CXX)
endif()

add_library(oxinode_ssd1306 STATIC
    src/Ssd1306.cpp
    src/Font5x7.cpp
)

target_include_directories(oxinode_ssd1306 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(oxinode_ssd1306 PUBLIC cxx_std_17)

set_target_properties(oxinode_ssd1306 PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# Same embedded-friendly contract as lib/max3010x: strict warnings,
# no exceptions or RTTI inside the driver. Kept PRIVATE so consumers
# (firmware OR host tests) make their own choice.
target_compile_options(oxinode_ssd1306 PRIVATE
    -Wall -Wextra -Werror
    -fno-exceptions -fno-rtti
)
