#include "max3010x/Max30102.hpp"

#include <cstring>

// MAX30102 driver. Datasheet references in comments are to the public
// MAX30102 datasheet (Rev 1, 10/2018). I/O is funnelled through IHal so
// the same code links into RP2040 and ESP32-S3 firmware.

namespace oxinode::max3010x
{
    static_assert(reg::kBytesPerEntrySpo2 == 6,
                  "SpO2 FIFO entry is RED(3) + IR(3) — 6 bytes total");
    static_assert(reg::kBytesPerEntryHr == 3,
                  "HR-only FIFO entry is a single 3-byte IR sample");

    Max30102::Max30102(IHal& hal)
        : m_hal(hal)
    {
    }

    int Max30102::probe()
    {
        uint8_t id = 0;
        const int rc = readReg(m_hal, m_cfg.devAddr, reg::PART_ID, id);
        if (rc != 0)
        {
            return -1;
        }
        if (id != reg::kPartId)
        {
            return -2;
        }
        return 0;
    }

    int Max30102::reset()
    {
        // MODE_CONFIG.RESET self-clears once the chip has finished —
        // datasheet says "all configuration, threshold and data
        // registers are reset to power-on state". Wait a few hundred
        // microseconds before continuing.
        const int rc = writeReg(m_hal, m_cfg.devAddr,
                                reg::MODE_CONFIG, reg::MODE_RESET);
        if (rc != 0)
        {
            return rc;
        }
        m_hal.delayUs(500);

        m_hr.reset();
        m_spo2.reset();
        m_configured = false;
        return 0;
    }

    int Max30102::shutdown()
    {
        // We OR SHDN into the current MODE_CONFIG so the operating
        // mode bits aren't lost — wakeup() will simply clear SHDN.
        uint8_t mode = 0;
        const int rc = readReg(m_hal, m_cfg.devAddr, reg::MODE_CONFIG, mode);
        if (rc != 0)
        {
            return rc;
        }
        return writeReg(m_hal, m_cfg.devAddr, reg::MODE_CONFIG,
                        static_cast<uint8_t>(mode | reg::MODE_SHUTDOWN));
    }

    int Max30102::wakeup()
    {
        uint8_t mode = 0;
        const int rc = readReg(m_hal, m_cfg.devAddr, reg::MODE_CONFIG, mode);
        if (rc != 0)
        {
            return rc;
        }
        return writeReg(m_hal, m_cfg.devAddr, reg::MODE_CONFIG,
                        static_cast<uint8_t>(mode & ~reg::MODE_SHUTDOWN));
    }

    int Max30102::configure(const Config& cfg)
    {
        m_cfg = cfg;

        int rc = reset();
        if (rc != 0)
        {
            return rc;
        }

        // ── FIFO_CONFIG: averaging | rollover | almost-full threshold
        // [7:5] SMP_AVE, [4] FIFO_ROLLOVER_EN, [3:0] FIFO_A_FULL.
        const uint8_t fifoCfg =
            static_cast<uint8_t>((static_cast<uint8_t>(cfg.avg) & 0x07) << 5) |
            static_cast<uint8_t>(cfg.fifoRollover ? (1 << 4) : 0) |
            static_cast<uint8_t>(cfg.fifoAlmostFullThreshold & 0x0F);
        rc = writeReg(m_hal, cfg.devAddr, reg::FIFO_CONFIG, fifoCfg);
        if (rc != 0) { return rc; }

        // ── SPO2_CONFIG: ADC range | sample rate | LED pulse width
        // [6:5] SPO2_ADC_RGE, [4:2] SPO2_SR, [1:0] LED_PW.
        const uint8_t spo2Cfg =
            static_cast<uint8_t>((static_cast<uint8_t>(cfg.adcRange)   & 0x03) << 5) |
            static_cast<uint8_t>((static_cast<uint8_t>(cfg.rate)       & 0x07) << 2) |
            static_cast<uint8_t>( static_cast<uint8_t>(cfg.pulseWidth) & 0x03);
        rc = writeReg(m_hal, cfg.devAddr, reg::SPO2_CONFIG, spo2Cfg);
        if (rc != 0) { return rc; }

        // ── LED currents
        rc = writeReg(m_hal, cfg.devAddr, reg::LED1_PA, cfg.redLedPa);
        if (rc != 0) { return rc; }
        rc = writeReg(m_hal, cfg.devAddr, reg::LED2_PA, cfg.irLedPa);
        if (rc != 0) { return rc; }

        // ── Clear FIFO pointers so the first interrupt reads from 0
        rc = writeReg(m_hal, cfg.devAddr, reg::FIFO_WR_PTR, 0);
        if (rc != 0) { return rc; }
        rc = writeReg(m_hal, cfg.devAddr, reg::OVF_COUNTER, 0);
        if (rc != 0) { return rc; }
        rc = writeReg(m_hal, cfg.devAddr, reg::FIFO_RD_PTR, 0);
        if (rc != 0) { return rc; }

        // ── Enable A_FULL + PPG_RDY interrupts. ALC_OVF stays off —
        // it just spams when the room lights flicker, the FIFO logic
        // handles it gracefully on its own.
        rc = writeReg(m_hal, cfg.devAddr, reg::INTR_ENABLE_1,
                      static_cast<uint8_t>(reg::INT_A_FULL | reg::INT_PPG_RDY));
        if (rc != 0) { return rc; }
        rc = writeReg(m_hal, cfg.devAddr, reg::INTR_ENABLE_2, 0);
        if (rc != 0) { return rc; }

        // ── MODE_CONFIG last: writing the mode arms the LEDs. Doing
        // it before the LED currents are programmed would briefly
        // flash whatever was on the chip from a previous power cycle.
        rc = writeReg(m_hal, cfg.devAddr, reg::MODE_CONFIG,
                      static_cast<uint8_t>(cfg.mode));
        if (rc != 0) { return rc; }

        m_configured = true;
        return 0;
    }

    void Max30102::decodeSpo2Entry(const uint8_t* p, uint32_t& red, uint32_t& ir)
    {
        // Datasheet §FIFO Data Format: each channel is 3 bytes,
        // MSB-first, top 6 bits of byte 0 read as 0; the low 18 bits
        // are the actual ADC count. Mask explicitly so callers can't
        // be surprised by phantom MSBs.
        red = (static_cast<uint32_t>(p[0]) << 16) |
              (static_cast<uint32_t>(p[1]) << 8)  |
               static_cast<uint32_t>(p[2]);
        red &= 0x0003FFFFu;

        ir  = (static_cast<uint32_t>(p[3]) << 16) |
              (static_cast<uint32_t>(p[4]) << 8)  |
               static_cast<uint32_t>(p[5]);
        ir &= 0x0003FFFFu;
    }

    void Max30102::decodeHrEntry(const uint8_t* p, uint32_t& ir)
    {
        ir  = (static_cast<uint32_t>(p[0]) << 16) |
              (static_cast<uint32_t>(p[1]) << 8)  |
               static_cast<uint32_t>(p[2]);
        ir &= 0x0003FFFFu;
    }

    int Max30102::handleInterrupt()
    {
        // Read both status registers in one go — the I²C peripheral
        // auto-increments past 0x00. We MUST read them to clear the
        // latched interrupt; the chip won't pull INT high otherwise.
        uint8_t status[2] = {};
        int rc = m_hal.i2cReadReg(m_cfg.devAddr, reg::INTR_STATUS_1,
                                  status, sizeof(status));
        if (rc != 0)
        {
            return -1;
        }

        // No FIFO event → nothing to do (e.g. PWR_RDY-only wake).
        const bool fifoEvent =
            (status[0] & (reg::INT_A_FULL | reg::INT_PPG_RDY)) != 0;
        if (!fifoEvent)
        {
            return 0;
        }

        // Compute number of unread entries from the pointer pair.
        // OVF_COUNTER bumps when the FIFO wrapped — we still drain
        // whatever's between RD and WR, but we surface the overflow
        // by skipping counts so callers can detect the gap.
        uint8_t ptrs[3] = {};
        rc = m_hal.i2cReadReg(m_cfg.devAddr, reg::FIFO_WR_PTR,
                              ptrs, sizeof(ptrs));
        if (rc != 0)
        {
            return -1;
        }
        const uint8_t wrPtr = ptrs[0] & 0x1F;
        const uint8_t ovf   = ptrs[1] & 0x1F;
        const uint8_t rdPtr = ptrs[2] & 0x1F;

        int unread = static_cast<int>(wrPtr) - static_cast<int>(rdPtr);
        if (unread < 0)
        {
            unread += reg::kFifoDepth;
        }
        // OVF != 0 means the chip wrapped; the unread count is then
        // the full FIFO. We log via the (non-existent) overflow
        // counter — for now just treat it as "drain everything".
        if (ovf > 0)
        {
            unread = reg::kFifoDepth;
        }
        if (unread == 0)
        {
            return 0;
        }

        const int bytesPerEntry =
            (m_cfg.mode == Mode::Spo2 || m_cfg.mode == Mode::MultiLed)
            ? reg::kBytesPerEntrySpo2
            : reg::kBytesPerEntryHr;

        const size_t toRead =
            static_cast<size_t>(unread) * static_cast<size_t>(bytesPerEntry);
        if (toRead > sizeof(m_burst))
        {
            // Defensive: should never happen given kFifoBurstBytes.
            return -3;
        }

        rc = m_hal.i2cReadReg(m_cfg.devAddr, reg::FIFO_DATA,
                              m_burst, toRead);
        if (rc != 0)
        {
            return -1;
        }

        const uint32_t tMs = m_hal.millis();
        for (int i = 0; i < unread; ++i)
        {
            uint32_t ir = 0;
            uint32_t red = 0;
            const uint8_t* p = &m_burst[i * bytesPerEntry];
            if (bytesPerEntry == reg::kBytesPerEntrySpo2)
            {
                decodeSpo2Entry(p, red, ir);
                m_spo2.push(ir, red);
            }
            else
            {
                decodeHrEntry(p, ir);
            }
            m_hr.push(tMs, static_cast<int32_t>(ir));
            notifySample(tMs, ir, red);
        }

        notifyHrSpo2(tMs);
        return unread;
    }

    int Max30102::readTemperatureC(float& out)
    {
        // Trigger a one-shot temp conversion. The chip clears TEMP_EN
        // automatically when done; ~29 ms typical.
        int rc = writeReg(m_hal, m_cfg.devAddr,
                          reg::TEMP_CONFIG, reg::TEMP_EN);
        if (rc != 0) { return rc; }

        // Spin until INTR_STATUS_2.DIE_TEMP_RDY trips, with a hard
        // ceiling so a stuck I²C bus can't hang us.
        for (int tries = 0; tries < 50; ++tries)
        {
            uint8_t s2 = 0;
            rc = readReg(m_hal, m_cfg.devAddr, reg::INTR_STATUS_2, s2);
            if (rc != 0) { return rc; }
            if (s2 & reg::INT_DIE_TEMP) { break; }
            m_hal.delayUs(1000);
        }

        uint8_t tInt  = 0;
        uint8_t tFrac = 0;
        rc = readReg(m_hal, m_cfg.devAddr, reg::TEMP_INT,  tInt);
        if (rc != 0) { return rc; }
        rc = readReg(m_hal, m_cfg.devAddr, reg::TEMP_FRAC, tFrac);
        if (rc != 0) { return rc; }

        // TEMP_INT is signed two's-complement; TEMP_FRAC is 0.0625 °C
        // per LSB (lower 4 bits valid).
        const int8_t signedInt = static_cast<int8_t>(tInt);
        out = static_cast<float>(signedInt) +
              (static_cast<float>(tFrac & 0x0F) * 0.0625f);
        return 0;
    }

    void Max30102::addObserver(ISampleObserver* obs)
    {
        if (obs == nullptr) { return; }
        if (m_observerCount >= kMaxObservers) { return; }
        m_observers[m_observerCount++] = obs;
    }

    void Max30102::notifySample(uint32_t tMs, uint32_t ir, uint32_t red)
    {
        for (int i = 0; i < m_observerCount; ++i)
        {
            m_observers[i]->onSample(tMs, ir, red);
        }
    }

    void Max30102::notifyHrSpo2(uint32_t tMs)
    {
        const uint8_t hr   = m_hr.bpm();
        const uint8_t spo2 = m_spo2.valid() ? m_spo2.spo2() : 0;
        for (int i = 0; i < m_observerCount; ++i)
        {
            m_observers[i]->onHrSpo2(tMs, hr, spo2);
        }
    }
}
