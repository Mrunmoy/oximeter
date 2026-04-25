// PHASE 2 — skeleton; first prototype runs on RP2040
#pragma once

#include <cstdint>
#include <cstddef>

#include "max3010x/IHal.hpp"

namespace oxinode::esp32
{
    // ── ESP-IDF v5.5 implementation of oxinode::max3010x::IHal ──
    //
    // Wires the four-method HAL contract to the new ESP-IDF
    // i2c_master_* API: a master bus handle plus a per-device handle
    // attached to that bus. The legacy `driver/i2c.h` API is NOT used
    // because it is being deprecated upstream.
    //
    // Error codes are negative POSIX errnos so the portable driver
    // can compare without pulling in any platform header. 50 ms per
    // transaction is generous for a 100 kHz bus moving the MAX30102's
    // ~17-sample FIFO bursts.
    class Esp32I2cHal final : public oxinode::max3010x::IHal
    {
    public:
        struct Config
        {
            int          i2cPort;       // 0 → I2C_NUM_0, 1 → I2C_NUM_1
            int          sdaGpio;       // GPIO number for SDA
            int          sclGpio;       // GPIO number for SCL
            std::uint32_t freqHz;        // Bus frequency, default 100 kHz
            std::uint8_t devAddr;       // 7-bit MAX30102 address (0x57)
        };

        explicit Esp32I2cHal(const Config& cfg);
        ~Esp32I2cHal();

        Esp32I2cHal(const Esp32I2cHal&)            = delete;
        Esp32I2cHal& operator=(const Esp32I2cHal&) = delete;
        Esp32I2cHal(Esp32I2cHal&&)                 = delete;
        Esp32I2cHal& operator=(Esp32I2cHal&&)      = delete;

        // ── Lifecycle ─────────────────────────────────────────────
        // Returns 0 on success, negative errno on failure. Idempotent:
        // a second call after a successful init is a no-op.
        [[nodiscard]] int init();

        // ── IHal contract ─────────────────────────────────────────
        [[nodiscard]] int i2cWriteReg(std::uint8_t devAddr, std::uint8_t reg,
                                      const std::uint8_t* data, std::size_t len) override;
        [[nodiscard]] int i2cReadReg(std::uint8_t devAddr, std::uint8_t reg,
                                     std::uint8_t* out, std::size_t len) override;
        [[nodiscard]] std::uint32_t millis() const override;
        void              delayUs(std::uint32_t us) override;

        // ── Convenience ───────────────────────────────────────────
        [[nodiscard]] std::uint8_t deviceAddress() const { return m_cfg.devAddr; }

    private:
        // ESP-IDF v5.5 uses opaque handle types
        // (i2c_master_bus_handle_t / i2c_master_dev_handle_t). Keep
        // them as void* in this header to avoid pulling
        // driver/i2c_master.h into every translation unit that
        // includes Esp32I2cHal.hpp.
        Config m_cfg;
        void*  m_busHandle;     // i2c_master_bus_handle_t
        void*  m_devHandle;     // i2c_master_dev_handle_t
        bool   m_inited;

        static constexpr std::uint32_t kI2cTimeoutMs = 50;
        static constexpr std::size_t   kCombinedBufBytes = 64;
    };
}
