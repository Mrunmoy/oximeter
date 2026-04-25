# ── App helper ──────────────────────────────────────────────────
#
# Wraps the boilerplate every OxiNode RP2040 application repeats:
#   - pico_add_extra_outputs (UF2 + map + bin + dis)
#   - USB-CDC stdio on, UART stdio off
#   - link the portable MAX3010x driver target
#   - inject OXINODE_BUILD_VERSION
#   - apply strict warning flags only to our sources (not pico-sdk /
#     tinyusb sources that the SDK injects via INTERFACE_SOURCES).
#
# Usage (from an app's CMakeLists.txt):
#
#     oxinode_add_app(oxinode
#         main.cpp
#         src/PicoI2cHal.cpp
#         src/PicoIntPin.cpp
#         src/UsbCdcLink.cpp
#     )

set(OXINODE_STRICT_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wdouble-promotion
    -Wformat=2
    -Wundef
    -Wconversion
    -Wsign-conversion
    $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
    $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
)

function(oxinode_add_app app_name)
    set(_sources ${ARGN})

    add_executable(${app_name} ${_sources})

    set_source_files_properties(${_sources} PROPERTIES
        COMPILE_OPTIONS "${OXINODE_STRICT_FLAGS}"
    )

    target_include_directories(${app_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
        ${CMAKE_SOURCE_DIR}/include
    )

    target_compile_definitions(${app_name} PRIVATE
        OXINODE_BUILD_VERSION="${OXINODE_BUILD_VERSION}"
    )

    target_link_libraries(${app_name} PRIVATE
        pico_stdlib
        pico_multicore
        hardware_i2c
        hardware_gpio
        hardware_irq
        hardware_timer
        oxinode_max3010x
    )

    # tinyusb_device gives us tud_cdc_n_* etc. Linked only when the
    # SDK actually has a tinyusb submodule (the flake-provided
    # pico-sdk pulls the submodule; the bare nixpkgs one does not).
    if(TARGET tinyusb_device)
        target_link_libraries(${app_name} PRIVATE tinyusb_device tinyusb_board)
    endif()

    pico_enable_stdio_usb(${app_name} 1)
    pico_enable_stdio_uart(${app_name} 0)

    pico_add_extra_outputs(${app_name})
endfunction()
