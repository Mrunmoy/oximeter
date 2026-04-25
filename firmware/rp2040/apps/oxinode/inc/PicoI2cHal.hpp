#pragma once

#include <cstdint>
#include <cstddef>

#include "max3010x/Hal.hpp"

namespace oxinode::rp2040
{
    // ── Pico-SDK implementation of oxinode::max3010x::IHal ──────
    //
    // Wires the four-method HAL contract to pico-sdk's hardware_i2c
    // driver and pico_time. The class is non-copyable, non-movable
    // and stateless beyond a non-owning pointer to the I²C peripheral
    // and the device address it talks to.
    //
    // All transactions use blocking-with-timeout pico-sdk calls
    // (50 ms default). Error codes are negative POSIX errnos
    // (-EIO, -ETIMEDOUT) so the portable driver can compare without
    // pulling in any platform header.
    class PicoI2cHal final : public oxinode::max3010x::IHal
    {
    public:
        struct Config
        {
            unsigned int i2cIndex;   // 0 → i2c0, 1 → i2c1
            unsigned int sdaPin;
            unsigned int sclPin;
            unsigned int freqHz;
            std::uint8_t devAddr;
        };

        explicit PicoI2cHal(const Config& cfg);

        PicoI2cHal(const PicoI2cHal&)            = delete;
        PicoI2cHal& operator=(const PicoI2cHal&) = delete;
        PicoI2cHal(PicoI2cHal&&)                 = delete;
        PicoI2cHal& operator=(PicoI2cHal&&)      = delete;

        // ── Lifecycle ───────────────────────────────────────────
        void init();

        // ── IHal contract ───────────────────────────────────────
        [[nodiscard]] int      i2cWriteReg(std::uint8_t devAddr, std::uint8_t reg,
                                           const std::uint8_t* data, std::size_t len) override;
        [[nodiscard]] int      i2cReadReg(std::uint8_t devAddr, std::uint8_t reg,
                                          std::uint8_t* out, std::size_t len) override;
        [[nodiscard]] std::uint32_t millis() const override;
        void                   delayUs(std::uint32_t us) override;

        // ── Convenience ─────────────────────────────────────────
        [[nodiscard]] std::uint8_t deviceAddress() const { return m_cfg.devAddr; }

    private:
        Config m_cfg;
        void*  m_i2c;     // i2c_inst_t*; void* keeps pico headers out of this header.
    };
}
