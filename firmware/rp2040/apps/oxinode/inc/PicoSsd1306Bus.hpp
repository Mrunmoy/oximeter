#pragma once

#include <cstddef>
#include <cstdint>

#include "ssd1306/IBus.hpp"

namespace oxinode::rp2040
{
    // Pico-SDK glue for `oxinode::ssd1306::IBus`.
    //
    // Owns its I²C peripheral (typically i2c1 on GP14/GP15 alongside
    // the MAX30102 on i2c0). The OLED driver is write-only, so this
    // class implements only the `write` half of an I²C bus contract.
    //
    // Stateless beyond the saved Config and the `i2c_inst_t*`. The
    // peripheral is initialised once in `init()`; subsequent `write`
    // calls are blocking-with-timeout.
    class PicoSsd1306Bus final : public oxinode::ssd1306::IBus
    {
    public:
        struct Config
        {
            unsigned int i2cIndex;   // 0 → i2c0, 1 → i2c1
            unsigned int sdaPin;
            unsigned int sclPin;
            unsigned int freqHz;
        };

        explicit PicoSsd1306Bus(const Config& cfg);

        PicoSsd1306Bus(const PicoSsd1306Bus&)            = delete;
        PicoSsd1306Bus& operator=(const PicoSsd1306Bus&) = delete;
        PicoSsd1306Bus(PicoSsd1306Bus&&)                 = delete;
        PicoSsd1306Bus& operator=(PicoSsd1306Bus&&)      = delete;

        // ── Lifecycle ───────────────────────────────────────────
        void init();

        // ── IBus contract ───────────────────────────────────────
        [[nodiscard]] int write(std::uint8_t devAddr,
                                const std::uint8_t* data,
                                std::size_t len) override;

    private:
        Config m_cfg;
        void*  m_i2c;   // i2c_inst_t* — kept as void* to keep the
                        // pico-sdk header out of this header file.
    };
}
