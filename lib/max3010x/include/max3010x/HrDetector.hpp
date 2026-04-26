#pragma once

#include <cstdint>

// Heart-rate estimator. Direct port of Maxim's MAXREFDES117# reference
// algorithm (algorithm.cpp::maxim_heart_rate_and_oxygen_saturation),
// described step-by-step in UG6409 §"Heart-Rate Post-Processing"
// (p.29-30) and §"SpO2 Post-Processing" (p.30-31). Pipeline:
//
//   raw IR ──▶ ring buffer (100 samples = 4 s @ 25 Hz)
//          ──▶ subtract buffer mean
//          ──▶ invert (peaks ↔ valleys, since systolic upstroke
//                       is a *valley* in the IR-ADC trace)
//          ──▶ 4-tap moving-average smoother
//          ──▶ find_peaks (height ≥ kMinPeakHeight,
//                          spacing ≥ kMinPeakDistance,
//                          max kMaxPeaks)
//          ──▶ BPM = (Fs · 60) / mean_peak_interval_samples
//
// The chip-side config (UG6409 Recommended Practices, p.26-27) is
// SR=100 Hz, SMP_AVE=4 → 25 Hz output. We recompute once per second
// (every kRecomputeEvery pushes) so the displayed BPM is a sliding
// 4-second average that updates at 1 Hz — same cadence the SparkFun
// MAX3010x example sketches print.
//
// All buffers are fixed size; no heap. Integer math throughout, since
// the RP2040 Cortex-M0+ has no FPU.

namespace oxinode::max3010x
{
    class HrDetector
    {
    public:
        // ── Tuning constants ────────────────────────────────────────
        // Output sample rate after on-chip averaging (Hz).
        static constexpr int kFs            = 25;
        // 4-second analysis window (Maxim default in algorithm.cpp).
        static constexpr int kBufferSec     = 4;
        static constexpr int kBufferSize    = kFs * kBufferSec;
        // 4-tap moving-average smoother — short enough that the systolic
        // upstroke is preserved, long enough to swallow photon noise.
        static constexpr int kMaSize        = 4;
        // Minimum peak-to-peak spacing in samples. 4 samples @ 25 Hz =
        // 0.16 s → 375 BPM ceiling. Anything tighter is noise.
        static constexpr int kMinPeakDistance = 4;
        // Hard floor on absolute peak amplitude (in inverted+demeaned
        // ADC counts). Maxim clamps to [30, 60] — we keep 30, which on
        // an 18-bit MAX30102 with finger on (AC swing ~2 K) is a
        // negligible gate, but on a no-finger trace (AC swing ~tens)
        // correctly rejects noise as having no real peaks.
        static constexpr int kMinPeakHeight = 30;
        // At most this many peaks per buffer. 4-second window × 240 BPM
        // ceiling = 16 beats; 15 is Maxim's value and we keep it.
        static constexpr int kMaxPeaks      = 15;
        // Recompute cadence: every kFs pushes = once per second of
        // input. Keeps CPU low and gives a stable, debounced display.
        static constexpr int kRecomputeEvery = kFs;
        // Plausibility band for a final BPM. Outside this range the
        // detector reports 0 ("not yet valid") rather than a number
        // it can't trust.
        static constexpr uint32_t kBpmMin = 30;
        static constexpr uint32_t kBpmMax = 240;
        // Median window applied to the recompute output. The Maxim
        // batch algorithm picks 4 s of peaks every 1 s; on a quietly-
        // resting finger it occasionally lands on a noisy peak set
        // and reports a one-cycle excursion before the next recompute
        // re-converges. A 3-deep median squashes any single-tick
        // outlier surrounded by stable readings — exactly the failure
        // pattern observed on the bench (2026-04-26: 88 BPM stable
        // for 90 % of readings, 1-second excursions to 68 / 93 / 115
        // for the remaining 10 %). Cost: at most kBpmMedianN-1 = 2 s
        // of extra latency on a real, sustained HR change. Acceptable
        // for steady-finger SpO2 use.
        static constexpr int kBpmMedianN = 3;

        HrDetector() = default;

        // Reset the entire pipeline — used by Max30102::reset() so a
        // hot-reconfigure doesn't carry over a stale beat history.
        void reset();

        // Push one IR sample. `tMs` is accepted for API compatibility
        // with the older streaming detector but is unused: the Maxim
        // batch algorithm operates in sample-index domain and computes
        // BPM from the implicit Fs.
        void push(uint32_t tMs, int32_t ir);

        // Latest BPM estimate. 0 means "not enough beats yet" or
        // "signal out of plausibility band" — platforms should treat
        // either as "no display".
        [[nodiscard]] uint8_t bpm() const { return m_bpm; }

        // Median of three uint8_t — pure function exposed publicly so
        // unit tests can pin its behaviour without round-tripping
        // through a synthetic signal. 0 is treated as a real value
        // (representing "not yet valid"); the median therefore acts as
        // a graceful fadeout when the finger is removed.
        [[nodiscard]] static uint8_t medianOf3(uint8_t a, uint8_t b, uint8_t c);

    private:
        // Run the Maxim algorithm over the current buffer contents.
        // Pushes the result through the median filter via pushOutput().
        void recompute();

        // Push one recompute result into the median ring and refresh
        // m_bpm from the median of the latest kBpmMedianN entries.
        void pushOutput(uint8_t bpm);

        // Find peaks in `signal[0..n)` that are at least `minHeight`
        // tall and at least `minDistance` samples apart. Output is
        // ascending sample indices in `outLocs`. Returns the number
        // of peaks (≤ kMaxPeaks). Faithfully follows
        // algorithm.cpp::maxim_find_peaks.
        static int findPeaks(int32_t* outLocs,
                             const int32_t* signal,
                             int n,
                             int32_t minHeight,
                             int minDistance);

        // ── Ring buffer of raw IR samples ──────────────────────────
        int32_t m_buf[kBufferSize] = {};
        int     m_filled = 0;          // 0..kBufferSize, then saturates
        int     m_writeIdx = 0;        // next write position
        int     m_sinceRecompute = 0;
        // ── Result ─────────────────────────────────────────────────
        uint8_t m_bpm = 0;
        // ── Median-of-N output filter ──────────────────────────────
        // Ring of the most recent recompute results (including the
        // 0 sentinel for "not yet valid"). m_bpm is the median over
        // the latest kBpmMedianN entries.
        uint8_t m_bpmHistory[kBpmMedianN] = {};
        int     m_bpmHistoryHead = 0;
        // ── Scratch (avoids stack pressure inside recompute) ───────
        int32_t m_work[kBufferSize] = {};
        int32_t m_locs[kMaxPeaks] = {};
    };
}
