#pragma once

#include <atomic>
#include <cstdint>

namespace oxinode::rp2040
{
    // ── INT line handler ────────────────────────────────────────
    //
    // The MAX30102 INT line is open-drain active-low. We configure
    // the GPIO as input with internal pull-up and arm a FALLING-edge
    // interrupt. The ISR is intentionally tiny:
    //
    //     1. bump m_edges (relaxed atomic)
    //     2. push a token into the inter-core FIFO
    //
    // No I²C, no logging, no allocation. The actual driver work
    // happens on core1's main loop, which blocks on the FIFO via
    // waitForInterrupt().
    class PicoIntPin
    {
    public:
        PicoIntPin()                             = delete;
        ~PicoIntPin()                            = delete;
        PicoIntPin(const PicoIntPin&)            = delete;
        PicoIntPin& operator=(const PicoIntPin&) = delete;

        // Configure the pin and arm the ISR. Idempotent.
        // Pass the GPIO number for the MAX30102 INT line.
        static void init(unsigned int pin);

        // Block the calling core until the ISR posts a token.
        // Intended for core1's main loop. Returns the running
        // edge counter (best-effort; counts may be coalesced if
        // edges arrive while we're handling a previous one).
        [[nodiscard]] static std::uint32_t waitForInterrupt();

        // Edge counter — useful for telemetry and watchdogs. Reads
        // are relaxed; the ISR uses memory_order_relaxed too.
        [[nodiscard]] static std::uint32_t edgeCount();

    private:
        // Pico-SDK ISR signature; declared static so we can install
        // it with gpio_set_irq_callback without a thunk.
        static void gpioIrqTrampoline(unsigned int gpio, std::uint32_t events);

        static std::atomic<std::uint32_t> s_edges;
        static unsigned int               s_pin;
        static bool                       s_inited;
    };
}
