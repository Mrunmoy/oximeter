# PHASE 2 — skeleton; first prototype runs on RP2040
#
# Component manifest for the OxiNode ESP32-S3-Zero firmware "main"
# component. Two things to flag:
#
#   1. We reach across the tree into ../../../lib/max3010x/ for the
#      portable driver core. The same pattern HeartNode uses for
#      third_party/regbus. Compiling the driver-core .cpp files into
#      the main component (rather than wrapping them in an IDF
#      component) keeps this skeleton minimal — once phase 2 grows we
#      can promote lib/max3010x to a proper IDF component with its own
#      idf_component_register call.
#
#   2. EMBED_FILES bakes index.html into flash; the WebSocketServer
#      hands it out at GET /. We do NOT use SPIFFS or LittleFS — the
#      single-file UI fits comfortably in the .rodata section.

set(OXINODE_DRIVER_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../lib/max3010x")

idf_component_register(
    SRCS
        "main.cpp"
        "src/Esp32I2cHal.cpp"
        "src/Esp32IntPin.cpp"
        "src/WifiManager.cpp"
        "src/WebSocketServer.cpp"
        "src/Heartbeat.cpp"
        "${OXINODE_DRIVER_DIR}/src/Max30102.cpp"
        "${OXINODE_DRIVER_DIR}/src/HrDetector.cpp"
        "${OXINODE_DRIVER_DIR}/src/Spo2Algo.cpp"
        "${OXINODE_DRIVER_DIR}/src/Framer.cpp"
    INCLUDE_DIRS
        "inc"
        "${OXINODE_DRIVER_DIR}/include"
    EMBED_FILES
        "index.html"
    PRIV_REQUIRES
        esp_wifi
        esp_http_server
        nvs_flash
        driver
        esp_netif
        esp_event
        esp_timer
        json
        esp_driver_i2c
        esp_driver_gpio
        freertos
)
