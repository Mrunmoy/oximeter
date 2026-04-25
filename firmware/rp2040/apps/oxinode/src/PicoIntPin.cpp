#include "PicoIntPin.hpp"

#include <atomic>
#include <cstdint>

#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "pico/multicore.h"

namespace oxinode::rp2040
{
    std::atomic<std::uint32_t> PicoIntPin::s_edges{0};
    unsigned int               PicoIntPin::s_pin{0};
    bool                       PicoIntPin::s_inited{false};

    namespace
    {
        // Sentinel value pushed into the FIFO on every edge. The
        // value itself doesn't matter; the wakeup semantics do.
        // We pick something distinctive so a future debug build can
        // sanity-check what's in the FIFO.
        static constexpr std::uint32_t kWakeToken = 0xA5A5'5A5Au;
    }

    void PicoIntPin::init(unsigned int pin)
    {
        if (s_inited)
        {
            return;
        }
        s_pin = pin;

        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        // Internal pull-up: the MAX30102 INT line is open-drain
        // active-low, so without this the line floats and we get
        // spurious edges from any breath of EMI.
        gpio_pull_up(pin);

        // Install our trampoline as the GPIO IRQ callback. The SDK
        // dispatches to it with (gpio_num, event_mask). FALLING_EDGE
        // matches the active-low INT pulse from the sensor.
        gpio_set_irq_enabled_with_callback(
            pin,
            GPIO_IRQ_EDGE_FALL,
            true,
            &PicoIntPin::gpioIrqTrampoline);

        s_inited = true;
    }

    std::uint32_t PicoIntPin::waitForInterrupt()
    {
        // multicore_fifo_pop_blocking puts the core to sleep until
        // the other core (or our own ISR) pushes a word into the
        // FIFO. Cheaper than polling and avoids the WFI / WFE dance.
        (void)multicore_fifo_pop_blocking();
        return s_edges.load(std::memory_order_relaxed);
    }

    bool PicoIntPin::waitForInterruptOrTimeout(std::uint32_t timeoutMs)
    {
        std::uint32_t token = 0;
        // multicore_fifo_pop_timeout_us takes microseconds; cap to
        // u32 to avoid overflow on large timeouts.
        const std::uint64_t us = static_cast<std::uint64_t>(timeoutMs) * 1000ULL;
        return multicore_fifo_pop_timeout_us(us, &token);
    }

    std::uint32_t PicoIntPin::edgeCount()
    {
        return s_edges.load(std::memory_order_relaxed);
    }

    void PicoIntPin::gpioIrqTrampoline(unsigned int gpio, std::uint32_t events)
    {
        // Defensive filter: the SDK shares one callback across all
        // GPIO IRQs, and we only own one pin here.
        if (gpio != s_pin || (events & GPIO_IRQ_EDGE_FALL) == 0)
        {
            return;
        }

        s_edges.fetch_add(1, std::memory_order_relaxed);

        // Push a wake token. multicore_fifo_push_timeout_us with a
        // zero timeout would be safer (FIFO is bounded to 8 words)
        // but the SDK doesn't expose a non-blocking variant directly
        // and the ISR rate is bounded by sample-rate / FIFO-depth.
        // If the FIFO is full it means core1 hasn't drained it yet —
        // dropping the wake is safe because s_edges has already been
        // bumped, so the next pop sees a fresh count.
        if (multicore_fifo_wready())
        {
            sio_hw->fifo_wr = kWakeToken;
        }
    }
}
