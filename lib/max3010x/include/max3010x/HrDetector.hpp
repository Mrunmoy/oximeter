#pragma once

#include <cstdint>

// Heart-rate estimator on AC-coupled IR samples. Pipeline (datasheet-
// agnostic — pure DSP):
//
//   raw IR ──▶ DC remover (single-pole IIR)
//          ──▶ moving-average smoother (kFilterTaps)
//          ──▶ threshold-crossing peak detector with refractory window
//          ──▶ rolling mean of last kIbiHistory inter-beat intervals → BPM
//
// Designed for 100 Hz sample rate (matches the default Max30102::Config).
// All buffers are fixed size — no heap.

namespace oxinode::max3010x
{
    class HrDetector
    {
    public:
        // ── Tuning constants ────────────────────────────────────────
        // 4-tap mean filter is enough at 100 Hz to swallow 50 Hz mains
        // ripple without smearing the systolic peak.
        static constexpr int kFilterTaps      = 4;
        // Average over the last 4 IBIs — short enough to track motion,
        // long enough to ignore single missed/extra beats.
        static constexpr int kIbiHistory      = 4;
        // 250 ms refractory matches a 240 BPM ceiling. Anything above
        // that is almost certainly noise on the rising edge.
        static constexpr uint32_t kRefractoryMs = 250;
        // DC-remover pole. α=0.95 at 100 Hz → ~3 dB cutoff ≈ 0.8 Hz,
        // well below the lowest plausible HR (30 BPM = 0.5 Hz)? Yes —
        // we accept the slight droop in exchange for fast settling.
        static constexpr float kDcAlpha       = 0.95f;
        // Adaptive threshold: peak must rise this fraction of the
        // running peak amplitude above the running trough to count.
        static constexpr float kPeakFraction  = 0.5f;

        HrDetector() = default;

        // Reset the entire pipeline — used by Max30102::reset() so a
        // hot-reconfigure doesn't carry over a stale beat history.
        void reset();

        // Push one IR sample; HR is updated in-place when a peak is
        // detected. `tMs` must be monotonic.
        void push(uint32_t tMs, int32_t ir);

        // Latest BPM estimate. 0 means "not enough beats yet" —
        // platforms should treat that as "no display".
        [[nodiscard]] uint8_t bpm() const { return m_bpm; }

    private:
        // ── DC remover state ────────────────────────────────────────
        float    m_prevX        = 0.0f;
        float    m_prevY        = 0.0f;
        bool     m_dcInit       = false;

        // ── Mean filter state ───────────────────────────────────────
        float    m_taps[kFilterTaps] = {};
        int      m_tapIdx       = 0;
        int      m_tapCount     = 0;

        // ── Adaptive envelope ───────────────────────────────────────
        float    m_envHigh      = 0.0f;
        float    m_envLow       = 0.0f;
        bool     m_envInit      = false;

        // ── Peak detector ───────────────────────────────────────────
        float    m_lastSmoothed = 0.0f;
        bool     m_aboveThresh  = false;
        uint32_t m_lastPeakMs   = 0;

        // ── IBI ring + result ───────────────────────────────────────
        uint32_t m_ibis[kIbiHistory] = {};
        int      m_ibiIdx       = 0;
        int      m_ibiCount     = 0;
        uint8_t  m_bpm          = 0;
    };
}
