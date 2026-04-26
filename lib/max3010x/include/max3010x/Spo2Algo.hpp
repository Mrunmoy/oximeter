#pragma once

#include <cstdint>

// Maxim ratio-of-ratios SpO2 estimator. Maintains a 100-sample rolling
// window of IR and RED samples (4 s at the chip's 25 Hz post-AVG_4
// output rate), computes AC RMS and DC mean per channel, then:
//
//   R    = (AC_red / DC_red) / (AC_ir / DC_ir)
//   SpO2 = a · R² + b · R + c   (AN6845 Table 1, p.13)
//
// Calibration coefficients are Maxim's published defaults for the
// MAX30101 / MAX30102 with no optical shield (kSpO2A, kSpO2B, kSpO2C
// below). UG6409 p.6 also gives a simpler linear approximation
// `SpO2 = 104 - 17R` from S. Prahl (1996); we keep that as the
// `kPrahlA / kPrahlB` constants for reference but use AN6845's
// quadratic by default since it is what Maxim ships as their
// calibrated curve for the MAXREFDES117# / MAX32664 reference design.
// Result is saturated to [0, 100].

namespace oxinode::max3010x
{
    class Spo2Algo
    {
    public:
        // 100 samples = 4 s at the chip's 25 Hz post-AVG_4 output. Long
        // enough that AC RMS contains several cardiac cycles even at
        // 30 BPM, short enough to track real saturation transients.
        static constexpr int kWindow = 100;

        // DC threshold below which the finger is considered missing.
        // The MAX30102 in normal operation reads >150 k counts on a
        // pressed finger (AN6845 step 8, p.9); ambient-light reading
        // on bare skin is < 5 k.
        static constexpr uint32_t kDcMinCounts = 5000;

        // AN6845 Table 1 (p.13) — calibrated SpO2 = aR² + bR + c for
        // the MAX30101 / MAX30102 with no optical shield. These are
        // Maxim's published defaults, derived from a 20-subject
        // controlled-O2 calibration in their lab.
        static constexpr double kSpO2A = 1.5958422;
        static constexpr double kSpO2B = -34.6596622;
        static constexpr double kSpO2C = 112.6898759;

        // UG6409 p.6 simple linear fallback (S. Prahl 1996); kept as
        // constants for clarity / cross-checking but not used unless
        // a caller swaps the curve at compile time.
        static constexpr double kPrahlA = -17.0;
        static constexpr double kPrahlB = 104.0;

        Spo2Algo() = default;

        void reset();

        void push(uint32_t ir, uint32_t red);

        // 0 if not enough samples buffered yet, or if finger is off.
        [[nodiscard]] uint8_t spo2() const { return m_spo2; }
        [[nodiscard]] bool valid() const { return m_valid; }

    private:
        // Two parallel ring buffers; we deliberately store as int32_t
        // so AC arithmetic is signed (counts fit comfortably).
        int32_t  m_ir[kWindow]  = {};
        int32_t  m_red[kWindow] = {};
        int      m_idx          = 0;
        int      m_count        = 0;

        uint8_t  m_spo2  = 0;
        bool     m_valid = false;
    };
}
