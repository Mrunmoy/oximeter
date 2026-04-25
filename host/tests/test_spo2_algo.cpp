#include "max3010x/Spo2Algo.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

namespace
{
    using oxinode::max3010x::Spo2Algo;

    constexpr double kPi = 3.14159265358979323846;

    // Feed a sinusoid into both channels with separately tunable AC
    // amplitudes — this is exactly the construction that makes the
    // Maxim ratio-of-ratios deterministic.
    //
    //   R = (AC_red/DC_red) / (AC_ir/DC_ir)
    //
    // Equal DC + matched RMS factor lets us hit any R we want.
    void feed(Spo2Algo& algo,
              double r,
              double dc,
              double acIr,
              int    samples)
    {
        // Pick AC_red such that R is exact:
        //   AC_red = R · (DC_red / DC_ir) · AC_ir
        // Same DC ⇒ AC_red = R · AC_ir.
        const double acRed = r * acIr;
        for (int i = 0; i < samples; ++i)
        {
            const double phase = (2.0 * kPi * i) / 25.0;  // ~1 Hz at 25 sps
            const double ir  = dc + acIr  * std::sin(phase);
            const double red = dc + acRed * std::sin(phase);
            algo.push(static_cast<uint32_t>(ir),
                      static_cast<uint32_t>(red));
        }
    }

    TEST(Spo2AlgoTest, RatioPoint4GivesSpo2Near100)
    {
        Spo2Algo algo;
        // R = 0.4 → 110 - 25·0.4 = 100. Saturates at 100 anyway.
        feed(algo, /*r=*/0.4, /*dc=*/100000.0, /*acIr=*/2000.0,
             Spo2Algo::kWindow);
        ASSERT_TRUE(algo.valid());
        EXPECT_GE(algo.spo2(), 99u);
        EXPECT_LE(algo.spo2(), 100u);
    }

    TEST(Spo2AlgoTest, RatioPoint8GivesSpo2Near90)
    {
        Spo2Algo algo;
        // R = 0.8 → 110 - 25·0.8 = 90.
        feed(algo, /*r=*/0.8, /*dc=*/100000.0, /*acIr=*/2000.0,
             Spo2Algo::kWindow);
        ASSERT_TRUE(algo.valid());
        EXPECT_GE(algo.spo2(), 89u);
        EXPECT_LE(algo.spo2(), 91u);
    }

    TEST(Spo2AlgoTest, RatioOnePoint4GivesSpo2Near75)
    {
        Spo2Algo algo;
        // R = 1.4 → 110 - 25·1.4 = 75.
        feed(algo, /*r=*/1.4, /*dc=*/100000.0, /*acIr=*/2000.0,
             Spo2Algo::kWindow);
        ASSERT_TRUE(algo.valid());
        EXPECT_GE(algo.spo2(), 74u);
        EXPECT_LE(algo.spo2(), 76u);
    }

    TEST(Spo2AlgoTest, FingerOffInvalid)
    {
        Spo2Algo algo;
        // DC well below kDcMinCounts (5000) — finger off.
        feed(algo, /*r=*/0.5, /*dc=*/1000.0, /*acIr=*/100.0,
             Spo2Algo::kWindow);
        EXPECT_FALSE(algo.valid());
        EXPECT_EQ(0u, algo.spo2());
    }

    TEST(Spo2AlgoTest, NotValidUntilWindowFull)
    {
        Spo2Algo algo;
        feed(algo, 0.5, 100000.0, 2000.0, Spo2Algo::kWindow / 2);
        EXPECT_FALSE(algo.valid());
    }

    TEST(Spo2AlgoTest, ResetClearsState)
    {
        Spo2Algo algo;
        feed(algo, 0.5, 100000.0, 2000.0, Spo2Algo::kWindow);
        ASSERT_TRUE(algo.valid());
        algo.reset();
        EXPECT_FALSE(algo.valid());
        EXPECT_EQ(0u, algo.spo2());
    }
}
