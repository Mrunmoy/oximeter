// PHASE 2 — skeleton; first prototype runs on RP2040
#include "Esp32I2cHal.hpp"

#include <cerrno>
#include <cstring>

#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

namespace oxinode::esp32
{
    namespace
    {
        constexpr const char* kTag = "Esp32I2cHal";

        // Map ESP-IDF esp_err_t values onto the negative-errno
        // convention the portable driver expects. The driver only
        // looks at sign + a couple of well-known codes; everything
        // else collapses to -EIO.
        [[nodiscard]] int mapEspErr(esp_err_t err)
        {
            switch (err)
            {
                case ESP_OK:              return 0;
                case ESP_ERR_TIMEOUT:     return -ETIMEDOUT;
                case ESP_ERR_INVALID_ARG: return -EINVAL;
                case ESP_ERR_NO_MEM:      return -ENOMEM;
                default:                  return -EIO;
            }
        }
    }

    Esp32I2cHal::Esp32I2cHal(const Config& cfg)
        : m_cfg(cfg)
        , m_busHandle(nullptr)
        , m_devHandle(nullptr)
        , m_inited(false)
    {
    }

    Esp32I2cHal::~Esp32I2cHal()
    {
        if (m_devHandle != nullptr)
        {
            (void)i2c_master_bus_rm_device(
                static_cast<i2c_master_dev_handle_t>(m_devHandle));
            m_devHandle = nullptr;
        }
        if (m_busHandle != nullptr)
        {
            (void)i2c_del_master_bus(
                static_cast<i2c_master_bus_handle_t>(m_busHandle));
            m_busHandle = nullptr;
        }
    }

    int Esp32I2cHal::init()
    {
        if (m_inited)
        {
            return 0;
        }

        // ── Bus handle ───────────────────────────────────────────
        i2c_master_bus_config_t busCfg{};
        busCfg.i2c_port           = m_cfg.i2cPort;
        busCfg.sda_io_num         = static_cast<gpio_num_t>(m_cfg.sdaGpio);
        busCfg.scl_io_num         = static_cast<gpio_num_t>(m_cfg.sclGpio);
        busCfg.clk_source         = I2C_CLK_SRC_DEFAULT;
        busCfg.glitch_ignore_cnt  = 7;
        busCfg.intr_priority      = 0;
        busCfg.trans_queue_depth  = 0;     // synchronous mode
        busCfg.flags.enable_internal_pullup = true;

        i2c_master_bus_handle_t busHandle = nullptr;
        esp_err_t err = i2c_new_master_bus(&busCfg, &busHandle);
        if (err != ESP_OK)
        {
            ESP_LOGE(kTag, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
            return mapEspErr(err);
        }
        m_busHandle = busHandle;

        // ── Device handle ────────────────────────────────────────
        i2c_device_config_t devCfg{};
        devCfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        devCfg.device_address  = m_cfg.devAddr;
        devCfg.scl_speed_hz    = m_cfg.freqHz;

        i2c_master_dev_handle_t devHandle = nullptr;
        err = i2c_master_bus_add_device(busHandle, &devCfg, &devHandle);
        if (err != ESP_OK)
        {
            ESP_LOGE(kTag, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
            (void)i2c_del_master_bus(busHandle);
            m_busHandle = nullptr;
            return mapEspErr(err);
        }
        m_devHandle = devHandle;

        m_inited = true;
        ESP_LOGI(kTag, "I2C ready: port=%d sda=%d scl=%d freq=%u Hz addr=0x%02X",
                 m_cfg.i2cPort, m_cfg.sdaGpio, m_cfg.sclGpio,
                 static_cast<unsigned>(m_cfg.freqHz), m_cfg.devAddr);
        return 0;
    }

    int Esp32I2cHal::i2cWriteReg(std::uint8_t devAddr, std::uint8_t reg,
                                 const std::uint8_t* data, std::size_t len)
    {
        if (!m_inited || m_devHandle == nullptr)
        {
            return -ENODEV;
        }
        if (data == nullptr && len > 0)
        {
            return -EINVAL;
        }
        if (devAddr != m_cfg.devAddr)
        {
            // The new IDF API binds devAddr at handle-add time.
            // Different addresses would need a second device handle —
            // the MAX30102 only has one, so this is an error.
            return -EINVAL;
        }
        if (len + 1u > kCombinedBufBytes)
        {
            return -E2BIG;
        }

        // Coalesce [reg | payload] into one transmit so the IDF
        // driver issues exactly one START / STOP. Required by the
        // MAX30102 register write protocol.
        std::uint8_t buf[kCombinedBufBytes];
        buf[0] = reg;
        if (len > 0)
        {
            std::memcpy(&buf[1], data, len);
        }

        const esp_err_t err = i2c_master_transmit(
            static_cast<i2c_master_dev_handle_t>(m_devHandle),
            buf,
            len + 1u,
            static_cast<int>(kI2cTimeoutMs));
        return mapEspErr(err);
    }

    int Esp32I2cHal::i2cReadReg(std::uint8_t devAddr, std::uint8_t reg,
                                std::uint8_t* out, std::size_t len)
    {
        if (!m_inited || m_devHandle == nullptr)
        {
            return -ENODEV;
        }
        if (out == nullptr || len == 0)
        {
            return -EINVAL;
        }
        if (devAddr != m_cfg.devAddr)
        {
            return -EINVAL;
        }

        // i2c_master_transmit_receive does write-then-read with a
        // repeated start, which is exactly the MAX30102 register-read
        // protocol: send register pointer, then read N bytes.
        const esp_err_t err = i2c_master_transmit_receive(
            static_cast<i2c_master_dev_handle_t>(m_devHandle),
            &reg, 1u,
            out, len,
            static_cast<int>(kI2cTimeoutMs));
        return mapEspErr(err);
    }

    std::uint32_t Esp32I2cHal::millis() const
    {
        // esp_timer_get_time returns int64 microseconds since boot.
        // Truncating to uint32 millis matches the contract — the
        // driver only uses millis() for relative timing windows.
        return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
    }

    void Esp32I2cHal::delayUs(std::uint32_t us)
    {
        // Busy-wait. For sub-millisecond precision the FreeRTOS tick
        // is too coarse. The driver only calls this for short waits
        // (< 1 ms) — power reset settling, mostly.
        esp_rom_delay_us(us);
    }
}
