#include "fakes/FakeI2cHal.hpp"
#include "max3010x/Framer.hpp"
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

    // ── readbackCfgCrc16 ────────────────────────────────────────

    TEST(Max30102Test, ReadbackCfgCrcReturnsZeroBeforeCalled)
    {
        FakeI2cHal hal;
        Max30102 dev(hal);
        ASSERT_EQ(0, dev.configure(Max30102::Config{}));
        // No readbackCfgCrc16 call yet — Stats::cfgCrc should still be 0.
        EXPECT_EQ(0u, dev.stats().cfgCrc);
    }

    TEST(Max30102Test, ReadbackCfgCrcMatchesExpectedRegisters)
    {
        FakeI2cHal hal;
        Max30102 dev(hal);
        ASSERT_EQ(0, dev.configure(Max30102::Config{}));

        uint16_t crc = 0;
        ASSERT_EQ(0, dev.readbackCfgCrc16(crc));

        // Independently rebuild what the readback should have CRC'd:
        // {INTR_ENABLE_1, INTR_ENABLE_2, FIFO_CONFIG, MODE_CONFIG,
        //  SPO2_CONFIG, LED1_PA, LED2_PA} — values that
        // ConfigureWritesExpectedRegisters already pins down.
        const uint8_t expected[7] = {
            hal.reg(reg::INTR_ENABLE_1),
            hal.reg(reg::INTR_ENABLE_2),
            hal.reg(reg::FIFO_CONFIG),
            hal.reg(reg::MODE_CONFIG),
            hal.reg(reg::SPO2_CONFIG),
            hal.reg(reg::LED1_PA),
            hal.reg(reg::LED2_PA),
        };
        const uint16_t expectedCrc =
            oxinode::max3010x::proto::crc16(expected, sizeof(expected));

        EXPECT_EQ(expectedCrc, crc);
        EXPECT_EQ(expectedCrc, dev.stats().cfgCrc);
    }

    TEST(Max30102Test, ReadbackCfgCrcChangesWhenChipRegistersChange)
    {
        FakeI2cHal hal;
        Max30102 dev(hal);
        ASSERT_EQ(0, dev.configure(Max30102::Config{}));

        uint16_t crc1 = 0;
        ASSERT_EQ(0, dev.readbackCfgCrc16(crc1));

        // Simulate the chip silently mangling its own config (e.g.
        // stuck-bit, half-brownout that didn't trip PWR_RDY): poke
        // an arbitrary bit in MODE_CONFIG behind the driver's back.
        hal.setReg(reg::MODE_CONFIG, hal.reg(reg::MODE_CONFIG) ^ 0x10);

        uint16_t crc2 = 0;
        ASSERT_EQ(0, dev.readbackCfgCrc16(crc2));
        EXPECT_NE(crc1, crc2);
    }

    TEST(Max30102Test, ReadbackCfgCrcPropagatesI2cFailure)
    {
        FakeI2cHal hal;
        Max30102 dev(hal);
        ASSERT_EQ(0, dev.configure(Max30102::Config{}));

        const Max30102::Stats before = dev.stats();
        hal.failNextRead();
        uint16_t crc = 0;
        EXPECT_NE(0, dev.readbackCfgCrc16(crc));
        // i2cErrTotal must have ticked.
        EXPECT_GT(dev.stats().i2cErrTotal, before.i2cErrTotal);
        // cfgCrc is left at whatever it was before — never half-written.
        EXPECT_EQ(before.cfgCrc, dev.stats().cfgCrc);
        // cfgCrcReadbacks is the validity-flag counter; a failed
        // readback must NOT bump it (host would otherwise see a
        // stale value flagged as fresh).
        EXPECT_EQ(before.cfgCrcReadbacks, dev.stats().cfgCrcReadbacks);
    }

    TEST(Max30102Test, ReadbackCfgCrcReadbacksCounterIsMonotonic)
    {
        // The counter exists as the unambiguous "have we run yet?"
        // signal — CRC-16 alone can be 0x0000 legitimately, so 0
        // would otherwise collide with the default sentinel. The
        // counter must be 0 before the first call and increment by
        // exactly 1 per successful call.
        FakeI2cHal hal;
        Max30102 dev(hal);
        ASSERT_EQ(0, dev.configure(Max30102::Config{}));
        EXPECT_EQ(0u, dev.stats().cfgCrcReadbacks);

        uint16_t crc = 0;
        ASSERT_EQ(0, dev.readbackCfgCrc16(crc));
        EXPECT_EQ(1u, dev.stats().cfgCrcReadbacks);

        ASSERT_EQ(0, dev.readbackCfgCrc16(crc));
        EXPECT_EQ(2u, dev.stats().cfgCrcReadbacks);

        ASSERT_EQ(0, dev.readbackCfgCrc16(crc));
        EXPECT_EQ(3u, dev.stats().cfgCrcReadbacks);
    }

    TEST(Max30102Test, HandleInterruptNoFlagsSetIsNoOp)
    {
        FakeI2cHal hal;
        Max30102 dev(hal);
        ASSERT_EQ(0, dev.configure(Max30102::Config{}));

        CapturingObserver obs;
        dev.addObserver(&obs);

        // Spurious wake — interrupt status is clear (e.g. another
        // process beat us to reading INTR_STATUS_1, or the line
        // glitched). Driver should burn one I²C read and return 0
        // without firing observers or touching counters.
        hal.setReg(reg::INTR_STATUS_1, 0);
        const Max30102::Stats before = dev.stats();
        EXPECT_EQ(0, dev.handleInterrupt());
        EXPECT_TRUE(obs.samples.empty());
        const Max30102::Stats after = dev.stats();
        EXPECT_EQ(before.samplesDrained, after.samplesDrained);
        EXPECT_EQ(before.pwrRdyEvents,   after.pwrRdyEvents);
        EXPECT_EQ(before.alcOvfEvents,   after.alcOvfEvents);
    }

    TEST(Max30102Test, HandleInterruptPwrRdyTriggersReconfigure)
    {
        FakeI2cHal hal;
        Max30102 dev(hal);
        ASSERT_EQ(0, dev.configure(Max30102::Config{}));

        CapturingObserver obs;
        dev.addObserver(&obs);

        // Simulate brownout-recovery: chip raises PWR_RDY. Datasheet
        // p.12-13 says config + FIFO state are gone; driver must
        // reconfigure before any drain is trustworthy.
        hal.setReg(reg::INTR_STATUS_1, reg::INT_PWR_RDY);
        hal.clearWrites();   // forget the configure() at line 1 of test

        EXPECT_EQ(0, dev.handleInterrupt());
        EXPECT_TRUE(obs.samples.empty());

        // Reconfigure path must have re-written the same register
        // block as the original configure(). Spot-check the load-
        // bearing ones.
        EXPECT_TRUE(hal.wasWritten(reg::FIFO_CONFIG));
        EXPECT_TRUE(hal.wasWritten(reg::SPO2_CONFIG));
        EXPECT_TRUE(hal.wasWritten(reg::LED1_PA));
        EXPECT_TRUE(hal.wasWritten(reg::LED2_PA));
        EXPECT_TRUE(hal.wasWritten(reg::INTR_ENABLE_1));
        EXPECT_TRUE(hal.wasWritten(reg::MODE_CONFIG));

        EXPECT_EQ(1u, dev.stats().pwrRdyEvents);
        EXPECT_EQ(reg::INT_PWR_RDY, dev.stats().lastInt1);
    }

    TEST(Max30102Test, HandleInterruptAlcOvfOnlyCountsButDoesNotDrain)
    {
        FakeI2cHal hal;
        Max30102 dev(hal);
        ASSERT_EQ(0, dev.configure(Max30102::Config{}));

        CapturingObserver obs;
        dev.addObserver(&obs);

        // Pure ALC_OVF — ambient light saturated, no FIFO event.
        // Counter ticks; no drain; no observer notification.
        hal.setReg(reg::INTR_STATUS_1, reg::INT_ALC_OVF);

        EXPECT_EQ(0, dev.handleInterrupt());
        EXPECT_TRUE(obs.samples.empty());
        EXPECT_EQ(1u, dev.stats().alcOvfEvents);
        EXPECT_EQ(0u, dev.stats().samplesDrained);
    }

    TEST(Max30102Test, HandleInterruptAlcOvfWithPpgRdyStillDrains)
    {
        FakeI2cHal hal;
        Max30102 dev(hal);
        ASSERT_EQ(0, dev.configure(Max30102::Config{}));

        CapturingObserver obs;
        dev.addObserver(&obs);

        // Bright room, but a sample is still pending. We count the
        // ALC_OVF (the sample is degraded) AND drain it — discarding
        // would lose data the application can still partially trust.
        hal.pushSpo2Entry(0x100, 0x200);
        hal.setReg(reg::INTR_STATUS_1,
                   static_cast<uint8_t>(reg::INT_ALC_OVF | reg::INT_PPG_RDY));
        hal.setReg(reg::FIFO_WR_PTR, 1);
        hal.setReg(reg::FIFO_RD_PTR, 0);
        hal.setReg(reg::OVF_COUNTER, 0);

        EXPECT_EQ(1, dev.handleInterrupt());
        EXPECT_EQ(1u, obs.samples.size());
        EXPECT_EQ(1u, dev.stats().alcOvfEvents);
        EXPECT_EQ(1u, dev.stats().samplesDrained);
    }

    TEST(Max30102Test, HandleInterruptAccumulatesChipOvfCounter)
    {
        FakeI2cHal hal;
        Max30102 dev(hal);
        ASSERT_EQ(0, dev.configure(Max30102::Config{}));

        CapturingObserver obs;
        dev.addObserver(&obs);

        for (int i = 0; i < 32; ++i)
        {
            hal.pushSpo2Entry(static_cast<uint32_t>(0x100 + i),
                              static_cast<uint32_t>(0x200 + i));
        }
        hal.setReg(reg::INTR_STATUS_1,
                   static_cast<uint8_t>(reg::INT_A_FULL | reg::INT_PPG_RDY));
        hal.setReg(reg::FIFO_WR_PTR, 0);
        hal.setReg(reg::FIFO_RD_PTR, 0);
        hal.setReg(reg::OVF_COUNTER, 5);   // chip wrapped 5 times

        EXPECT_EQ(32, dev.handleInterrupt());
        EXPECT_EQ(5u,  dev.stats().chipOvfTotal);
        EXPECT_EQ(32u, dev.stats().samplesDrained);

        // A second drain — this time *without* overflow — proves
        // both counters accumulate (chipOvfTotal stays put, drained
        // adds 4). chipOvfTotal monotonicity gets exercised in the
        // dedicated test below.
        for (int i = 0; i < 4; ++i)
        {
            hal.pushSpo2Entry(static_cast<uint32_t>(0x300 + i),
                              static_cast<uint32_t>(0x400 + i));
        }
        hal.setReg(reg::INTR_STATUS_1, reg::INT_PPG_RDY);
        hal.setReg(reg::FIFO_WR_PTR, 4);
        hal.setReg(reg::FIFO_RD_PTR, 0);
        hal.setReg(reg::OVF_COUNTER, 0);

        EXPECT_EQ(4, dev.handleInterrupt());
        EXPECT_EQ(5u,  dev.stats().chipOvfTotal);     // unchanged (no new OVF)
        EXPECT_EQ(36u, dev.stats().samplesDrained);   // 32 + 4
    }

    TEST(Max30102Test, HandleInterruptOvfTotalIsMonotonic)
    {
        FakeI2cHal hal;
        Max30102 dev(hal);
        ASSERT_EQ(0, dev.configure(Max30102::Config{}));

        CapturingObserver obs;
        dev.addObserver(&obs);

        // Two consecutive overflow events. Whenever OVF_COUNTER > 0
        // the driver treats the FIFO as full (32 entries), so we
        // pre-stage 32 entries each round.
        for (int round = 0; round < 2; ++round)
        {
            for (int i = 0; i < 32; ++i)
            {
                hal.pushSpo2Entry(static_cast<uint32_t>(round * 0x100 + i),
                                  static_cast<uint32_t>(round * 0x200 + i));
            }
            hal.setReg(reg::INTR_STATUS_1,
                       static_cast<uint8_t>(reg::INT_A_FULL | reg::INT_PPG_RDY));
            hal.setReg(reg::FIFO_WR_PTR, 0);
            hal.setReg(reg::FIFO_RD_PTR, 0);
            hal.setReg(reg::OVF_COUNTER, static_cast<uint8_t>(3 + round));
            EXPECT_EQ(32, dev.handleInterrupt());
        }
        EXPECT_EQ(7u, dev.stats().chipOvfTotal);   // 3 + 4
    }

    TEST(Max30102Test, HandleInterruptI2cFailureBumpsErrorCounter)
    {
        FakeI2cHal hal;
        Max30102 dev(hal);
        ASSERT_EQ(0, dev.configure(Max30102::Config{}));

        const uint32_t before = dev.stats().i2cErrTotal;
        hal.failNextRead();
        EXPECT_EQ(-1, dev.handleInterrupt());
        EXPECT_EQ(before + 1u, dev.stats().i2cErrTotal);
    }

    TEST(Max30102Test, StatsStartAtZero)
    {
        FakeI2cHal hal;
        Max30102 dev(hal);
        const Max30102::Stats s = dev.stats();
        EXPECT_EQ(0u, s.samplesDrained);
        EXPECT_EQ(0u, s.chipOvfTotal);
        EXPECT_EQ(0u, s.pwrRdyEvents);
        EXPECT_EQ(0u, s.alcOvfEvents);
        EXPECT_EQ(0u, s.i2cErrTotal);
        EXPECT_EQ(0u, s.lastInt1);
        EXPECT_EQ(0u, s.lastInt2);
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
