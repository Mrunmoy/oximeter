#include "PicoSsd1306Bus.hpp"

#include <cerrno>

#include "hardware/gpio.h"
#include "hardware/i2c.h"

namespace oxinode::rp2040
{
    namespace
    {
        // 50 ms per transaction is generous: the largest single write
        // we issue is 33 bytes (1 control + 32 data) which at 400 kHz
        // takes ~750 µs. The padding covers clock-stretching, panel
        // glitches, and shared-bus contention that may show up if a
        // future revision moves the OLED onto i2c0 with the MAX30102.
        static constexpr std::uint32_t kI2cTimeoutUs = 50u * 1000u;

        [[nodiscard]] i2c_inst_t* picoI2cFromIndex(unsigned int index)
        {
            return (index == 0u) ? i2c0 : i2c1;
        }
    }

    PicoSsd1306Bus::PicoSsd1306Bus(const Config& cfg)
        : m_cfg(cfg)
        , m_i2c(picoI2cFromIndex(cfg.i2cIndex))
    {
    }

    void PicoSsd1306Bus::init()
    {
        i2c_inst_t* const inst = static_cast<i2c_inst_t*>(m_i2c);

        (void)i2c_init(inst, m_cfg.freqHz);

        gpio_set_function(m_cfg.sdaPin, GPIO_FUNC_I2C);
        gpio_set_function(m_cfg.sclPin, GPIO_FUNC_I2C);

        // OLED breakouts in the bring-up bag carry their own pull-ups,
        // but enabling internal pull-ups means a missing module still
        // parks the bus at idle-high — a clean failure mode if the
        // panel is unplugged.
        gpio_pull_up(m_cfg.sdaPin);
        gpio_pull_up(m_cfg.sclPin);
    }

    int PicoSsd1306Bus::write(std::uint8_t devAddr,
                              const std::uint8_t* data,
                              std::size_t len)
    {
        if (data == nullptr || len == 0u)
        {
            return -EINVAL;
        }

        // The driver hands us a contiguous [control_byte | payload]
        // buffer, so a single i2c_write_timeout_us is exactly what we
        // need — no need for a coalescing scratch like the MAX30102
        // path uses (that path takes a separate `reg` argument).
        i2c_inst_t* const inst = static_cast<i2c_inst_t*>(m_i2c);

        const int rc = i2c_write_timeout_us(
            inst,
            devAddr,
            data,
            len,
            /*nostop=*/false,
            kI2cTimeoutUs);

        if (rc == PICO_ERROR_TIMEOUT)
        {
            return -ETIMEDOUT;
        }
        if (rc < 0)
        {
            return -EIO;
        }
        if (static_cast<std::size_t>(rc) != len)
        {
            return -EIO;
        }
        return 0;
    }
}
