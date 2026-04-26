#include "max3010x/Spo2Algo.hpp"

#include <cmath>

namespace oxinode::max3010x
{
    void Spo2Algo::reset()
    {
        for (int i = 0; i < kWindow; ++i)
        {
            m_ir[i]  = 0;
            m_red[i] = 0;
        }
        m_idx   = 0;
        m_count = 0;
        m_spo2  = 0;
        m_valid = false;
    }

    void Spo2Algo::push(uint32_t ir, uint32_t red)
    {
        m_ir[m_idx]  = static_cast<int32_t>(ir);
        m_red[m_idx] = static_cast<int32_t>(red);
        m_idx = (m_idx + 1) % kWindow;
        if (m_count < kWindow) { ++m_count; }

        if (m_count < kWindow)
        {
            // Still filling — don't publish a number we don't trust.
            m_valid = false;
            m_spo2  = 0;
            return;
        }

        // ── DC means ─────────────────────────────────────────────────
        int64_t sumIr  = 0;
        int64_t sumRed = 0;
        for (int i = 0; i < kWindow; ++i)
        {
            sumIr  += m_ir[i];
            sumRed += m_red[i];
        }
        const double meanIr  = static_cast<double>(sumIr)  / static_cast<double>(kWindow);
        const double meanRed = static_cast<double>(sumRed) / static_cast<double>(kWindow);

        // ── Finger-off detection ─────────────────────────────────────
        // Bare-skin/ambient reads are far below kDcMinCounts. We also
        // bail if either DC is non-positive — that would make the
        // ratio degenerate.
        if (meanIr < static_cast<double>(kDcMinCounts) ||
            meanRed < static_cast<double>(kDcMinCounts))
        {
            m_valid = false;
            m_spo2  = 0;
            return;
        }

        // ── AC RMS ───────────────────────────────────────────────────
        // RMS over a window that contains > 1 cardiac cycle is a good
        // proxy for AC amplitude — the Maxim AN6409 reference does the
        // same thing. Variance form keeps numerics tidy at large DC.
        double sqIr  = 0.0;
        double sqRed = 0.0;
        for (int i = 0; i < kWindow; ++i)
        {
            const double dIr  = static_cast<double>(m_ir[i])  - meanIr;
            const double dRed = static_cast<double>(m_red[i]) - meanRed;
            sqIr  += dIr  * dIr;
            sqRed += dRed * dRed;
        }
        const double rmsIr  = std::sqrt(sqIr  / static_cast<double>(kWindow));
        const double rmsRed = std::sqrt(sqRed / static_cast<double>(kWindow));

        // Avoid divide-by-zero on a perfectly DC signal (synthetic
        // tests sometimes hit this).
        if (rmsIr <= 0.0)
        {
            m_valid = false;
            m_spo2  = 0;
            return;
        }

        const double r = (rmsRed / meanRed) / (rmsIr / meanIr);
        // AN6845 Table 1 calibrated quadratic (p.13).
        double spo2 = kSpO2A * r * r + kSpO2B * r + kSpO2C;
        if (spo2 > 100.0) { spo2 = 100.0; }
        if (spo2 < 0.0)   { spo2 = 0.0;   }

        m_spo2  = static_cast<uint8_t>(spo2 + 0.5);
        m_valid = true;
    }
}
