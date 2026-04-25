#include "max3010x/HrDetector.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

namespace
{
    using oxinode::max3010x::HrDetector;

    constexpr double kPi = 3.14159265358979323846;

    // Generate a simple sinusoidal IR PPG-like signal: large positive
    // DC plus a moderate AC component at the target heart rate.
    void feedSine(HrDetector& d, double bpm,
                  double dcCounts, double acCounts,
                  double sampleRateHz, double durationSec)
    {
        const int totalSamples =
            static_cast<int>(sampleRateHz * durationSec);
        const double freqHz = bpm / 60.0;
        for (int i = 0; i < totalSamples; ++i)
        {
            const double t = i / sampleRateHz;
            const double v = dcCounts +
                             acCounts * std::sin(2.0 * kPi * freqHz * t);
            const uint32_t tMs =
                static_cast<uint32_t>(1000.0 * t);
            d.push(tMs, static_cast<int32_t>(v));
        }
    }

    TEST(HrDetectorTest, ConvergesTo60Bpm)
    {
        HrDetector d;
        // 5 seconds at 100 Hz of 60 BPM signal → 5 beats. With 4-IBI
        // averaging the BPM should be solidly close to 60 by the end.
        feedSine(d, /*bpm=*/60.0, /*dc=*/100000.0, /*ac=*/2000.0,
                 /*sr=*/100.0, /*sec=*/8.0);
        EXPECT_GE(d.bpm(), 58u);
        EXPECT_LE(d.bpm(), 62u);
    }

    TEST(HrDetectorTest, FlatSignalGivesZero)
    {
        HrDetector d;
        for (int i = 0; i < 500; ++i)
        {
            d.push(static_cast<uint32_t>(i * 10), 50000);
        }
        EXPECT_EQ(0u, d.bpm());
    }

    TEST(HrDetectorTest, FastSignal180Bpm)
    {
        HrDetector d;
        feedSine(d, /*bpm=*/180.0, /*dc=*/100000.0, /*ac=*/2000.0,
                 /*sr=*/100.0, /*sec=*/6.0);
        EXPECT_GE(d.bpm(), 170u);
        EXPECT_LE(d.bpm(), 190u);
    }

    TEST(HrDetectorTest, ResetClearsHistory)
    {
        HrDetector d;
        feedSine(d, 60.0, 100000.0, 2000.0, 100.0, 6.0);
        ASSERT_GT(d.bpm(), 0u);
        d.reset();
        EXPECT_EQ(0u, d.bpm());
    }
}
