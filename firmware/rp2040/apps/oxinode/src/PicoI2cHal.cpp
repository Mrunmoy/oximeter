#include "PicoI2cHal.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/time.h"

namespace oxinode::rp2040
{
    namespace
    {
        // 50 ms per transaction is generous for a 100 kHz bus moving
        // at most ~32 bytes (FIFO burst is ~17 samples * 6 bytes = 102
        // bytes). At 100 kHz that's ~12 ms; the headroom covers
        // clock-stretching by the slave under heavy LED-pulse load.
        static constexpr std::uint32_t kI2cTimeoutUs = 50u * 1000u;

        // Combined-buffer size: one register byte + the largest
        // payload we ever push (sensor INT enable / mode config words
        // are well under 32 bytes). Sized comfortably to avoid the
        // need for a heap allocation when callers send long bursts.
        static constexpr std::size_t  kCombinedBufBytes = 64;

        [[nodiscard]] i2c_inst_t* picoI2cFromIndex(unsigned int index)
        {
            return (index == 0) ? i2c0 : i2c1;
        }
    }

    PicoI2cHal::PicoI2cHal(const Config& cfg)
        : m_cfg(cfg)
        , m_i2c(picoI2cFromIndex(cfg.i2cIndex))
    {
    }

    void PicoI2cHal::init()
    {
        i2c_inst_t* const inst = static_cast<i2c_inst_t*>(m_i2c);

        // SDK helper sets the actual baud (returns the rate it
        // achieved); we don't gate on it because the MAX30102 is
        // forgiving about ±10 % bus-clock drift.
        (void)i2c_init(inst, m_cfg.freqHz);

        gpio_set_function(m_cfg.sdaPin, GPIO_FUNC_I2C);
        gpio_set_function(m_cfg.sclPin, GPIO_FUNC_I2C);

        // Defense in depth: the GY-style breakout has 4.7 kΩ
        // pull-ups, but enabling internal pull-ups means a missing /
        // disconnected breakout still parks the bus at idle-high
        // instead of floating, which matters for ESD on bench wiring.
        gpio_pull_up(m_cfg.sdaPin);
        gpio_pull_up(m_cfg.sclPin);
    }

    int PicoI2cHal::i2cWriteReg(std::uint8_t devAddr, std::uint8_t reg,
                                const std::uint8_t* data, std::size_t len)
    {
        if (data == nullptr && len > 0)
        {
            return -EINVAL;
        }
        if (len + 1u > kCombinedBufBytes)
        {
            return -E2BIG;
        }

        // Pico-SDK's i2c_write_blocking issues exactly one START.
        // The MAX30102 expects [reg | payload] in a single
        // transaction (no repeated start between reg and data), so
        // we coalesce into a small stack buffer. No dynamic alloc.
        std::uint8_t buf[kCombinedBufBytes];
        buf[0] = reg;
        if (len > 0)
        {
            std::memcpy(&buf[1], data, len);
        }

        i2c_inst_t* const inst = static_cast<i2c_inst_t*>(m_i2c);
        const int rc = i2c_write_timeout_us(
            inst,
            devAddr,
            buf,
            len + 1u,
            false,             // nostop = false → release the bus
            kI2cTimeoutUs);

        if (rc == PICO_ERROR_TIMEOUT)
        {
            return -ETIMEDOUT;
        }
        if (rc < 0)
        {
            return -EIO;
        }
        if (static_cast<std::size_t>(rc) != (len + 1u))
        {
            return -EIO;
        }
        return 0;
    }

    int PicoI2cHal::i2cReadReg(std::uint8_t devAddr, std::uint8_t reg,
                               std::uint8_t* out, std::size_t len)
    {
        if (out == nullptr || len == 0)
        {
            return -EINVAL;
        }

        i2c_inst_t* const inst = static_cast<i2c_inst_t*>(m_i2c);

        // Phase 1: write register pointer with nostop=true so the
        // SDK does NOT issue a STOP — the read below then re-arms
        // the bus with a repeated START, which is what every I²C
        // device with auto-increment register access expects.
        const int wrc = i2c_write_timeout_us(
            inst,
            devAddr,
            &reg,
            1,
            true,              // nostop = true → repeated start
            kI2cTimeoutUs);

        if (wrc == PICO_ERROR_TIMEOUT)
        {
            return -ETIMEDOUT;
        }
        if (wrc != 1)
        {
            return -EIO;
        }

        const int rrc = i2c_read_timeout_us(
            inst,
            devAddr,
            out,
            len,
            false,             // nostop = false → STOP after read
            kI2cTimeoutUs);

        if (rrc == PICO_ERROR_TIMEOUT)
        {
            return -ETIMEDOUT;
        }
        if (rrc < 0)
        {
            return -EIO;
        }
        if (static_cast<std::size_t>(rrc) != len)
        {
            return -EIO;
        }
        return 0;
    }

    std::uint32_t PicoI2cHal::millis() const
    {
        return to_ms_since_boot(get_absolute_time());
    }

    void PicoI2cHal::delayUs(std::uint32_t us)
    {
        sleep_us(us);
    }
}
