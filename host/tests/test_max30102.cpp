#include "fakes/FakeI2cHal.hpp"
#include "max3010x/Max30102.hpp"
#include "max3010x/Registers.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace
{
    using oxinode::max3010x::Max30102;
    using oxinode::max3010x::ISampleObserver;
    namespace reg = oxinode::max3010x::reg;
    using oxinode::test::FakeI2cHal;

    struct CapturingObserver : public ISampleObserver
    {
        struct Sample { uint32_t t; uint32_t ir; uint32_t red; };

        void onSample(uint32_t tMs, uint32_t ir, uint32_t red) override
        {
            samples.push_back({tMs, ir, red});
        }
        void onHrSpo2(uint32_t tMs, uint8_t hr, uint8_t spo2) override
        {
            hrSpo2.push_back({tMs, hr, spo2});
        }

        struct HrSpo2 { uint32_t t; uint8_t hr; uint8_t spo2; };
        std::vector<Sample> samples;
        std::vector<HrSpo2> hrSpo2;
    };

    TEST(Max30102Test, ProbeSucceedsOnCorrectPartId)
    {
        FakeI2cHal hal;  // default-seeded with PART_ID = 0x15
        Max30102 dev(hal);
        EXPECT_EQ(0, dev.probe());
    }

    TEST(Max30102Test, ProbeFailsOnWrongPartId)
    {
        FakeI2cHal hal;
        hal.setReg(reg::PART_ID, 0xAA);
        Max30102 dev(hal);
        EXPECT_EQ(-2, dev.probe());
    }

    TEST(Max30102Test, ProbeFailsOnI2cError)
    {
        FakeI2cHal hal;
        hal.failNextRead();
        Max30102 dev(hal);
        EXPECT_EQ(-1, dev.probe());
    }

    TEST(Max30102Test, ConfigureWritesExpectedRegisters)
    {
        FakeI2cHal hal;
        Max30102 dev(hal);
        ASSERT_EQ(0, dev.configure(Max30102::Config{}));

        // Reset must have happened before the per-field writes.
        EXPECT_TRUE(hal.wasWritten(reg::MODE_CONFIG));
        EXPECT_TRUE(hal.wasWritten(reg::FIFO_CONFIG));
        EXPECT_TRUE(hal.wasWritten(reg::SPO2_CONFIG));
        EXPECT_TRUE(hal.wasWritten(reg::LED1_PA));
        EXPECT_TRUE(hal.wasWritten(reg::LED2_PA));
        EXPECT_TRUE(hal.wasWritten(reg::FIFO_WR_PTR));
        EXPECT_TRUE(hal.wasWritten(reg::FIFO_RD_PTR));
        EXPECT_TRUE(hal.wasWritten(reg::OVF_COUNTER));
        EXPECT_TRUE(hal.wasWritten(reg::INTR_ENABLE_1));

        // Final MODE_CONFIG should reflect SpO2 mode (0x03).
        EXPECT_EQ(reg::MODE_SPO2, hal.reg(reg::MODE_CONFIG));

        // SPO2_CONFIG default: ADC range 4096 (0b01) | rate 100 (0b001)
        // | pulsewidth 411 (0b11) → (0b01<<5) | (0b001<<2) | 0b11
        //   = 0b00100111 = 0x27.
        EXPECT_EQ(0x27, hal.reg(reg::SPO2_CONFIG));

        // FIFO_CONFIG: avg 4 (0b010<<5) | rollover (1<<4) | thresh 0x0F
        //   = 0b01011111 = 0x5F.
        EXPECT_EQ(0x5F, hal.reg(reg::FIFO_CONFIG));

        // INTR_ENABLE_1: A_FULL | PPG_RDY = 0xC0.
        EXPECT_EQ(0xC0, hal.reg(reg::INTR_ENABLE_1));
    }

    TEST(Max30102Test, ResetClearsState)
    {
        FakeI2cHal hal;
        Max30102 dev(hal);
        ASSERT_EQ(0, dev.configure(Max30102::Config{}));
        ASSERT_EQ(0, dev.reset());
        // Reset issues MODE_CONFIG = 0x40 (RESET bit). The chip would
        // self-clear it; our fake leaves it as written, which is fine.
        EXPECT_EQ(reg::MODE_RESET, hal.reg(reg::MODE_CONFIG));
        EXPECT_EQ(0u, dev.bpm());
        EXPECT_EQ(0u, dev.spo2());
    }

    TEST(Max30102Test, HandleInterruptDrainsFifoAndFiresObservers)
    {
        FakeI2cHal hal;
        Max30102 dev(hal);
        ASSERT_EQ(0, dev.configure(Max30102::Config{}));

        CapturingObserver obs;
        dev.addObserver(&obs);

        // Pre-stage 5 SpO2 entries with predictable values so we can
        // assert order.
        const std::vector<std::pair<uint32_t, uint32_t>> samples = {
            {0x0000010, 0x0000020},  // red, ir
            {0x0000110, 0x0000120},
            {0x0000210, 0x0000220},
            {0x0000310, 0x0000320},
            {0x0000410, 0x0000420},
        };
        for (const auto& s : samples)
        {
            hal.pushSpo2Entry(s.first, s.second);
        }

        // Status: PPG_RDY set, no overflow.
        hal.setReg(reg::INTR_STATUS_1, reg::INT_PPG_RDY);
        // FIFO pointers say "5 entries unread".
        hal.setReg(reg::FIFO_WR_PTR, 5);
        hal.setReg(reg::FIFO_RD_PTR, 0);
        hal.setReg(reg::OVF_COUNTER, 0);

        const int n = dev.handleInterrupt();
        ASSERT_EQ(5, n);
        ASSERT_EQ(5u, obs.samples.size());
        for (size_t i = 0; i < samples.size(); ++i)
        {
            EXPECT_EQ(samples[i].first,  obs.samples[i].red) << "i=" << i;
            EXPECT_EQ(samples[i].second, obs.samples[i].ir)  << "i=" << i;
        }
    }

    TEST(Max30102Test, HandleInterruptNoEventDoesNothing)
    {
        FakeI2cHal hal;
        Max30102 dev(hal);
        ASSERT_EQ(0, dev.configure(Max30102::Config{}));

        CapturingObserver obs;
        dev.addObserver(&obs);

        // Only PWR_RDY set — no FIFO traffic to drain.
        hal.setReg(reg::INTR_STATUS_1, reg::INT_PWR_RDY);

        EXPECT_EQ(0, dev.handleInterrupt());
        EXPECT_TRUE(obs.samples.empty());
    }

    TEST(Max30102Test, HandleInterruptHandlesOverflowGracefully)
    {
        FakeI2cHal hal;
        Max30102 dev(hal);
        ASSERT_EQ(0, dev.configure(Max30102::Config{}));

        CapturingObserver obs;
        dev.addObserver(&obs);

        // Pre-stage a full FIFO worth (32 entries × 6 bytes = 192).
        for (int i = 0; i < 32; ++i)
        {
            hal.pushSpo2Entry(static_cast<uint32_t>(0x100 + i),
                              static_cast<uint32_t>(0x200 + i));
        }

        hal.setReg(reg::INTR_STATUS_1,
                   static_cast<uint8_t>(reg::INT_A_FULL | reg::INT_PPG_RDY));
        hal.setReg(reg::FIFO_WR_PTR, 0);
        hal.setReg(reg::FIFO_RD_PTR, 0);
        hal.setReg(reg::OVF_COUNTER, 5);   // chip says it wrapped 5 times

        const int n = dev.handleInterrupt();
        EXPECT_EQ(32, n);
        EXPECT_EQ(32u, obs.samples.size());
    }

    TEST(Max30102Test, ObserverCapEnforced)
    {
        FakeI2cHal hal;
        Max30102 dev(hal);
        CapturingObserver obs[Max30102::kMaxObservers + 2];
        for (int i = 0; i < Max30102::kMaxObservers + 2; ++i)
        {
            dev.addObserver(&obs[i]);
        }
        // Past kMaxObservers the calls are silently dropped — assert
        // that adding a sixth doesn't blow up by drainging samples.
        ASSERT_EQ(0, dev.configure(Max30102::Config{}));
        hal.setReg(reg::INTR_STATUS_1, reg::INT_PPG_RDY);
        hal.setReg(reg::FIFO_WR_PTR, 1);
        hal.pushSpo2Entry(100, 200);
        EXPECT_EQ(1, dev.handleInterrupt());
        // First kMaxObservers observers received the sample; the
        // overflow ones did not.
        EXPECT_EQ(1u, obs[0].samples.size());
        EXPECT_EQ(1u, obs[Max30102::kMaxObservers - 1].samples.size());
        EXPECT_EQ(0u, obs[Max30102::kMaxObservers].samples.size());
    }
}
