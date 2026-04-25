#pragma once

#include "max3010x/Hal.hpp"
#include "max3010x/Registers.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

// FakeI2cHal — a header-only IHal that pretends to be a MAX30102. It
// exposes a flat 256-byte register file plus a separate FIFO byte queue
// (since reads from FIFO_DATA come from a stream, not a register slot).
// Tests freely read/write the register file and push canned FIFO bytes.
//
// Test code is NOT subject to the lib's "no STL" rule — this fake uses
// std::deque/vector for ergonomics.

namespace oxinode::test
{
    class FakeI2cHal : public oxinode::max3010x::IHal
    {
    public:
        FakeI2cHal()
        {
            m_regs.fill(0);
            // Sensible defaults so probe() succeeds without seeding.
            m_regs[oxinode::max3010x::reg::PART_ID] =
                oxinode::max3010x::reg::kPartId;
            m_regs[oxinode::max3010x::reg::REV_ID] = 0x05;
        }

        // ── IHal ────────────────────────────────────────────────────
        int i2cWriteReg(uint8_t /*devAddr*/, uint8_t reg,
                        const uint8_t* data, size_t len) override
        {
            if (m_failNextWrite)
            {
                m_failNextWrite = false;
                return -1;
            }
            for (size_t i = 0; i < len; ++i)
            {
                const size_t addr = (static_cast<size_t>(reg) + i) & 0xFF;
                m_regs[addr] = data[i];
                m_writeLog.push_back({static_cast<uint8_t>(addr), data[i]});
            }
            return 0;
        }

        int i2cReadReg(uint8_t /*devAddr*/, uint8_t reg,
                       uint8_t* out, size_t len) override
        {
            if (m_failNextRead)
            {
                m_failNextRead = false;
                return -1;
            }
            // FIFO_DATA pops from the canned byte queue so tests can
            // simulate streaming sample bursts.
            if (reg == oxinode::max3010x::reg::FIFO_DATA)
            {
                for (size_t i = 0; i < len; ++i)
                {
                    if (m_fifo.empty())
                    {
                        out[i] = 0;
                    }
                    else
                    {
                        out[i] = m_fifo.front();
                        m_fifo.pop_front();
                    }
                }
                return 0;
            }
            for (size_t i = 0; i < len; ++i)
            {
                const size_t addr = (static_cast<size_t>(reg) + i) & 0xFF;
                out[i] = m_regs[addr];
            }
            return 0;
        }

        uint32_t millis() const override { return m_millis; }

        void delayUs(uint32_t us) override
        {
            // Roll the fake clock forward a millisecond at most so
            // unit tests can observe time advancing across busy waits
            // without having to manually tick.
            m_millis += (us + 999) / 1000;
        }

        // ── Test helpers ────────────────────────────────────────────
        uint8_t reg(uint8_t addr) const { return m_regs[addr]; }
        void setReg(uint8_t addr, uint8_t val) { m_regs[addr] = val; }

        void pushFifoByte(uint8_t b) { m_fifo.push_back(b); }
        void pushFifoBytes(const uint8_t* p, size_t n)
        {
            for (size_t i = 0; i < n; ++i) { m_fifo.push_back(p[i]); }
        }

        // Helper to pre-stage one SpO2 entry (RED first, then IR; each
        // 3 bytes MSB-first, low 18 bits valid).
        void pushSpo2Entry(uint32_t red, uint32_t ir)
        {
            m_fifo.push_back(static_cast<uint8_t>((red >> 16) & 0x03));
            m_fifo.push_back(static_cast<uint8_t>((red >> 8)  & 0xFF));
            m_fifo.push_back(static_cast<uint8_t>( red        & 0xFF));
            m_fifo.push_back(static_cast<uint8_t>((ir  >> 16) & 0x03));
            m_fifo.push_back(static_cast<uint8_t>((ir  >> 8)  & 0xFF));
            m_fifo.push_back(static_cast<uint8_t>( ir         & 0xFF));
        }

        void advanceMs(uint32_t ms) { m_millis += ms; }
        void setMs(uint32_t ms)     { m_millis  = ms; }

        struct WriteEvent { uint8_t addr; uint8_t value; };
        const std::vector<WriteEvent>& writes() const { return m_writeLog; }
        void clearWrites() { m_writeLog.clear(); }

        bool wasWritten(uint8_t addr) const
        {
            for (const WriteEvent& w : m_writeLog)
            {
                if (w.addr == addr) { return true; }
            }
            return false;
        }

        size_t fifoSize() const { return m_fifo.size(); }

        void failNextRead()  { m_failNextRead  = true; }
        void failNextWrite() { m_failNextWrite = true; }

    private:
        std::array<uint8_t, 256> m_regs{};
        std::deque<uint8_t>      m_fifo{};
        std::vector<WriteEvent>  m_writeLog{};
        uint32_t                 m_millis        = 0;
        bool                     m_failNextRead  = false;
        bool                     m_failNextWrite = false;
    };
}
