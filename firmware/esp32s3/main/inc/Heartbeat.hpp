// PHASE 2 — skeleton; first prototype runs on RP2040
#pragma once

namespace oxinode::esp32
{
    // ── 1 Hz "alive" indicator on the on-board WS2812 ─────────────
    //
    // The Waveshare ESP32-S3-Zero ships with a single addressable
    // NeoPixel hard-wired to GPIO21. Driving it via the IDF managed
    // component `led_strip` is the cleanest path, but if the
    // component isn't pulled in we degrade to a once-per-second
    // ESP_LOGI("alive") so the user can still see the firmware is
    // running on the serial monitor.
    //
    // The task lives on whichever core FreeRTOS picks
    // (tskNO_AFFINITY); priority is low.
    class Heartbeat
    {
    public:
        Heartbeat()                            = delete;
        ~Heartbeat()                           = delete;
        Heartbeat(const Heartbeat&)            = delete;
        Heartbeat& operator=(const Heartbeat&) = delete;

        static void start();

        // GPIO21 is the WS2812 data line on the S3-Zero.
        static constexpr int kStatusLedGpio = 21;
    };
}
