#pragma once

#include <cstdint>

// Maxim ratio-of-ratios SpO2 estimator. Maintains a rolling 1-second
// window (100 samples at 100 Hz) of IR and RED, computes AC RMS and DC
// mean per channel, then:
//
//   R    = (AC_red / DC_red) / (AC_ir / DC_ir)
//   SpO2 = 110 - 25 · R     (Maxim reference linear approximation)
//
// Saturated at 100. The full Maxim AN6409 polynomial is more accurate
// but only marginally so for the 70-100 % range we care about; the
// linear form is what most open-source MAX30102 drivers use and what
// this project's docs commit to.

namespace oxinode::max3010x
{
    class Spo2Algo
    {
    public:
        // 1 second of samples at the default 100 Hz rate. Enough cycles
        // (≥ 1 heartbeat) for AC RMS to be well-defined.
        static constexpr int kWindow = 100;

        // DC threshold below which the finger is considered missing.
        // The MAX30102 in normal operation reads ~50 k counts on a
        // pressed finger; ambient-light reading on bare skin is < 5 k.
        static constexpr uint32_t kDcMinCounts = 5000;

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
