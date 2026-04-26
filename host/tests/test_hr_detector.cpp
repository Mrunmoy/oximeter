#include "max3010x/HrDetector.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

// Tests for the HR detector. The implementation is a port of Maxim's
// reference HR algorithm shipped with the MAXREFDES117# evaluation
// design and described in UG6409 §"Heart-Rate Post-Processing" (p.29-30):
//
//   1. Buffer 100 samples (4 s at the chip's 25 Hz post-AVG_4 output).
//   2. Subtract the buffer mean and invert (so peak-finder operates as
//      a valley-finder, since a PPG systolic upstroke is a *valley*
//      in the IR-ADC trace as more blood absorbs more light).
//   3. 4-tap moving-average smoother (kills mains hum + photon noise).
//   4. find_peaks above a hard-floor height threshold, separated by at
//      least 4 samples (≈0.16 s, which caps detected HR at ~375 BPM).
//   5. BPM = (Fs × 60) / mean_peak_interval_samples.
//
// All synthetic inputs feed at 25 Hz to match the production sample
// rate (UG6409 Recommended Configurations: SR=100 Hz, SMP_AVE=4).

namespace
{
    using oxinode::max3010x::HrDetector;

    constexpr double kPi = 3.14159265358979323846;
    constexpr double kFsHz = 25.0;

    // Generate a synthetic PPG-like signal: large positive DC plus an
    // AC component at the target heart rate.
    void feedSine(HrDetector& d, double bpm,
                  double dcCounts, double acCounts,
                  double durationSec)
    {
        const int totalSamples =
            static_cast<int>(kFsHz * durationSec);
        const double freqHz = bpm / 60.0;
        for (int i = 0; i < totalSamples; ++i)
        {
            const double t = i / kFsHz;
            const double v = dcCounts +
                             acCounts * std::sin(2.0 * kPi * freqHz * t);
            const uint32_t tMs =
                static_cast<uint32_t>(1000.0 * t);
            d.push(tMs, static_cast<int32_t>(v));
        }
    }

    // 60 BPM is the canonical reference: a 1 Hz fundamental with
    // ~5 beats per 4-second buffer.
    TEST(HrDetectorTest, ConvergesTo60Bpm)
    {
        HrDetector d;
        feedSine(d, /*bpm=*/60.0, /*dc=*/150000.0, /*ac=*/2000.0,
                 /*sec=*/8.0);
        EXPECT_GE(d.bpm(), 56u);
        EXPECT_LE(d.bpm(), 64u);
    }

    TEST(HrDetectorTest, FlatSignalGivesZero)
    {
        HrDetector d;
        // 8 s of dead-flat DC — no AC, no peaks above height threshold.
        for (int i = 0; i < 200; ++i)
        {
            d.push(static_cast<uint32_t>(i * 40), 50000);
        }
        EXPECT_EQ(0u, d.bpm());
    }

    // 180 BPM = 3 Hz, period = 8.33 samples at 25 Hz. Comfortably
    // above the 4-sample min-peak-distance (= ~375 BPM ceiling).
    TEST(HrDetectorTest, FastSignal180Bpm)
    {
        HrDetector d;
        feedSine(d, /*bpm=*/180.0, /*dc=*/150000.0, /*ac=*/2000.0,
                 /*sec=*/6.0);
        EXPECT_GE(d.bpm(), 168u);
        EXPECT_LE(d.bpm(), 192u);
    }

    // Bradycardia (40 BPM) — the previous envelope-tracker port locked
    // at -1 here; the buffered Maxim algorithm should handle it cleanly.
    TEST(HrDetectorTest, SlowSignal40Bpm)
    {
        HrDetector d;
        feedSine(d, /*bpm=*/40.0, /*dc=*/150000.0, /*ac=*/2000.0,
                 /*sec=*/12.0);
        EXPECT_GE(d.bpm(), 36u);
        EXPECT_LE(d.bpm(), 44u);
    }

    // BPM is unavailable until the 100-sample (4 s @ 25 Hz) window has
    // been filled. Half-filled → caller sees 0, not a wrong number.
    TEST(HrDetectorTest, BufferNotFullGivesZero)
    {
        HrDetector d;
        feedSine(d, 60.0, 150000.0, 2000.0, /*sec=*/2.0);
        EXPECT_EQ(0u, d.bpm());
    }

    // 50 Hz mains ripple is far above the PPG band (Maxim AN6410 puts
    // the signal band at 0–20 Hz). The 4-tap MA should knock it down
    // hard enough that a 60 BPM signal still locks.
    // (At Fs=25 Hz, "50 Hz noise" aliases — we use 8 Hz instead, which
    // is well into the noise band but representable.)
    TEST(HrDetectorTest, HighFrequencyNoiseRejected)
    {
        HrDetector d;
        const double dur = 12.0;
        const int totalSamples =
            static_cast<int>(kFsHz * dur);
        for (int i = 0; i < totalSamples; ++i)
        {
            const double t = i / kFsHz;
            const double signal =
                150000.0 + 2000.0 * std::sin(2.0 * kPi * 1.0 * t);
            const double noise =
                500.0 * std::sin(2.0 * kPi * 8.0 * t);
            d.push(static_cast<uint32_t>(1000.0 * t),
                   static_cast<int32_t>(signal + noise));
        }
        EXPECT_GE(d.bpm(), 56u);
        EXPECT_LE(d.bpm(), 64u);
    }

    TEST(HrDetectorTest, ResetClearsHistory)
    {
        HrDetector d;
        feedSine(d, 60.0, 150000.0, 2000.0, /*sec=*/8.0);
        ASSERT_GT(d.bpm(), 0u);
        d.reset();
        EXPECT_EQ(0u, d.bpm());
    }

    // ── Output median filter ─────────────────────────────────────

    TEST(HrDetectorTest, MedianOf3IsBranchOrderInvariant)
    {
        // The median of 3 values is independent of the order they are
        // presented in. Regression guard for the branchless form in
        // HrDetector::medianOf3.
        EXPECT_EQ(60u, HrDetector::medianOf3(60u, 60u, 60u));
        EXPECT_EQ(60u, HrDetector::medianOf3(60u, 60u, 115u));
        EXPECT_EQ(60u, HrDetector::medianOf3(60u, 115u, 60u));
        EXPECT_EQ(60u, HrDetector::medianOf3(115u, 60u, 60u));
        EXPECT_EQ(60u, HrDetector::medianOf3(0u, 60u, 115u));
        EXPECT_EQ(60u, HrDetector::medianOf3(115u, 60u, 0u));
        EXPECT_EQ(0u,  HrDetector::medianOf3(0u, 0u, 60u));
        EXPECT_EQ(60u, HrDetector::medianOf3(0u, 60u, 60u));
    }

    TEST(HrDetectorTest, MedianOf5IsOrderInvariant)
    {
        // Branchless 5-element median exercised with the same kind of
        // adversarial reorderings used for medianOf3, plus a few that
        // specifically exercise the 2-consecutive-bad-sample case
        // that motivated the bump from 3 to 5.
        EXPECT_EQ(60u, HrDetector::medianOf5(60u, 60u, 60u, 60u, 60u));
        EXPECT_EQ(60u, HrDetector::medianOf5(60u, 60u, 60u, 60u, 100u));
        EXPECT_EQ(60u, HrDetector::medianOf5(60u, 60u, 60u, 100u, 100u));
        EXPECT_EQ(60u, HrDetector::medianOf5(100u, 100u, 60u, 60u, 60u));
        EXPECT_EQ(60u, HrDetector::medianOf5(100u, 60u, 60u, 100u, 60u));
        EXPECT_EQ(60u, HrDetector::medianOf5(0u, 60u, 60u, 60u, 100u));
        EXPECT_EQ(0u,  HrDetector::medianOf5(0u, 0u, 0u, 60u, 60u));
        // Median of three bad samples (≥ N/2 + 1) DOES flip — that's
        // the per-design failure threshold of an N-deep median.
        EXPECT_EQ(100u, HrDetector::medianOf5(60u, 60u, 100u, 100u, 100u));
    }

    TEST(HrDetectorTest, MedianFilterAbsorbsBriefSignalDropout)
    {
        // The pattern observed on bench: stable 60 BPM for several
        // seconds, then one or two recompute windows where the
        // chip's input is briefly noisy and the algorithm reports
        // 0 / a wrong value, then back to 60. With the median-of-5
        // output filter, that 1-2 cycle dropout should NOT show up
        // as a 0 in `bpm()`; it should remain ≈60.
        HrDetector d;
        // 8 s of clean 60 BPM → buffer fills, ≥5 recomputes converge
        // on 60. After this, the median ring is `[60, 60, 60, 60, 60]`
        // and `bpm()` is fully warmed up.
        feedSine(d, 60.0, 150000.0, 2000.0, /*sec=*/8.0);
        ASSERT_GE(d.bpm(), 56u);
        ASSERT_LE(d.bpm(), 64u);

        // Inject 1 s of flat (DC-only) signal. The next recompute
        // sees a buffer with 3/4 clean + 1/4 flat; in the worst case
        // it reports 0, which lands in a `[0,60,60,60,60]` ring
        // whose median is still 60.
        for (int i = 0; i < HrDetector::kRecomputeEvery; ++i)
        {
            d.push(/*tMs=*/0u, /*ir=*/150000);
        }
        EXPECT_GE(d.bpm(), 56u) << "median should hold previous stable BPM";
        EXPECT_LE(d.bpm(), 64u);
    }

    TEST(HrDetectorTest, MedianFilterAbsorbsTwoConsecutiveBadRecomputes)
    {
        // Bench observation 2026-04-27: consecutive recomputes share
        // 3 s of input data, so a noise burst can poison **two**
        // adjacent recomputes — the failure mode median-of-3 could
        // not absorb (median([good, bad, bad]) = bad). The new
        // median-of-5 reduces the bad samples to a 2-of-5 minority,
        // so the median holds.
        //
        // We don't have a clean way to force the algorithm to emit
        // exactly two adjacent bad BPMs from a synthetic signal, so
        // instead we feed enough clean data to fill the ring with
        // 60 BPM, then independently verify the pure-function math:
        // 3 good + 2 bad → median is good.
        HrDetector d;
        feedSine(d, 60.0, 150000.0, 2000.0, /*sec=*/8.0);
        ASSERT_GE(d.bpm(), 56u);
        ASSERT_LE(d.bpm(), 64u);

        // Same shape verification at the medianOf5 level, on the same
        // values the algorithm actually produces in practice.
        EXPECT_EQ(60u, HrDetector::medianOf5(60u, 60u, 60u, 100u, 100u));
        EXPECT_EQ(60u, HrDetector::medianOf5(100u, 100u, 60u, 60u, 60u));
    }

    TEST(HrDetectorTest, MedianFilterFadesOutAfterSustainedDropout)
    {
        // Complementary to the above: a *sustained* loss of signal
        // (finger removed) should drag bpm() down to 0 within a
        // bounded number of recompute cycles, not stick at the last
        // valid reading forever.
        //
        // Two phases needed for full fadeout:
        //   1. Fill the 4 s analysis buffer with flat signal so the
        //      Maxim algorithm finds no real peaks → recompute = 0.
        //   2. kBpmMedianN further recomputes pushing 0 to drain the
        //      median ring of any residual valid history.
        // Total: kBufferSec + kBpmMedianN seconds of flat input.
        HrDetector d;
        // 8 s feed (not 5 s) — at 25 Hz the median ring takes roughly
        // ceil(kBpmMedianN/2)+1 recomputes (i.e. ~3-4 s post-buffer-
        // fill) to flip from the 0-init majority to the live signal.
        feedSine(d, 60.0, 150000.0, 2000.0, /*sec=*/8.0);
        ASSERT_GE(d.bpm(), 56u);

        const int samples =
            HrDetector::kRecomputeEvery *
            (HrDetector::kBufferSec + HrDetector::kBpmMedianN);
        for (int i = 0; i < samples; ++i)
        {
            d.push(/*tMs=*/0u, /*ir=*/150000);
        }
        EXPECT_EQ(0u, d.bpm());
    }
}
