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
        hardware_watchdog
        oxinode_max3010x
        oxinode_ssd1306
    )

    # USB-CDC: pico_enable_stdio_usb brings pico_stdio_usb (and
    # tinyusb_device transitively) with the SDK's known-good USB
    # descriptor. Do NOT also link tinyusb_board — its standalone
    # board-config descriptor conflicts with pico_stdio_usb's and
    # the host won't enumerate. If we ever want raw tud_cdc_n_*
    # access we'll roll a custom descriptor and drop pico_stdio_usb.
    pico_enable_stdio_usb(${app_name} 1)
    pico_enable_stdio_uart(${app_name} 0)

    pico_add_extra_outputs(${app_name})

    # Post-link sanity check: the SPSC sample ring (`g_ring`) lives in
    # the RP2040 scratch_x SRAM bank for cross-core perf reasons (see
    # comment on g_ring in main.cpp; DESIGN.md follow-up to D-15).
    # If a future change drops the `__scratch_x` attribute or another
    # variable displaces the ring out of bank 4, fail the firmware
    # build right at the point of regression instead of letting the
    # perf claim quietly rot.
    add_custom_command(TARGET ${app_name} POST_BUILD
        COMMAND bash
                ${CMAKE_SOURCE_DIR}/../../scripts/check-scratch-x.sh
                $<TARGET_FILE:${app_name}>
        COMMENT "Verifying g_ring placement in scratch_x SRAM bank"
        VERBATIM
    )
endfunction()
