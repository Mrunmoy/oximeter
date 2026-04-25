// PHASE 2 — skeleton; first prototype runs on RP2040
#pragma once

#include <atomic>
#include <cstdint>

namespace oxinode::esp32
{
    // ── INT line handler ─────────────────────────────────────────
    //
    // The MAX30102 INT line is open-drain active-low. The pin is
    // configured as input with the ESP32-S3 internal pull-up enabled
    // (the GY-style breakout already has a 4.7 kΩ external pull-up,
    // but the internal one is cheap insurance against ESD on a bare
    // wire). FALLING-edge interrupt only.
    //
    // The ISR is a five-line wonder:
    //
    //     1. bump m_edges (relaxed atomic)
    //     2. xSemaphoreGiveFromISR on a binary semaphore
    //     3. yield if a higher-priority task was woken
    //
    // No I²C, no logging, no allocation. The sensor task waits on
    // the semaphore from outside the ISR and drains the FIFO there.
    class Esp32IntPin
    {
    public:
        Esp32IntPin()                              = delete;
        ~Esp32IntPin()                             = delete;
        Esp32IntPin(const Esp32IntPin&)            = delete;
        Esp32IntPin& operator=(const Esp32IntPin&) = delete;

        // Configure the pin and arm the ISR. Returns 0 on success,
        // negative errno on failure. Idempotent.
        [[nodiscard]] static int init(int gpioNum);

        // Block the calling task on the binary semaphore until the
        // ISR posts. timeoutMs == 0xFFFFFFFF means wait forever.
        // Returns true on wakeup, false on timeout.
        [[nodiscard]] static bool waitForInterrupt(std::uint32_t timeoutMs = 0xFFFFFFFFu);

        [[nodiscard]] static std::uint32_t edgeCount();

    private:
        // Defined in Esp32IntPin.cpp with IRAM_ATTR so the ISR sits
        // in IRAM (cache misses on flash would tank our edge timing).
        static void gpioIsrTrampoline(void* arg);

        static std::atomic<std::uint32_t> s_edges;
        static int                        s_gpio;
        static bool                       s_inited;
        static void*                      s_sem;   // SemaphoreHandle_t
    };
}
