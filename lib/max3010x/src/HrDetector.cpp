#include "max3010x/HrDetector.hpp"

#include <cmath>

namespace oxinode::max3010x
{
    void HrDetector::reset()
    {
        m_prevX = 0.0f;
        m_prevY = 0.0f;
        m_dcInit = false;

        for (int i = 0; i < kFilterTaps; ++i) { m_taps[i] = 0.0f; }
        m_tapIdx = 0;
        m_tapCount = 0;

        m_envHigh = 0.0f;
        m_envLow  = 0.0f;
        m_envInit = false;

        m_lastSmoothed = 0.0f;
        m_aboveThresh = false;
        m_lastPeakMs = 0;

        for (int i = 0; i < kIbiHistory; ++i) { m_ibis[i] = 0; }
        m_ibiIdx = 0;
        m_ibiCount = 0;
        m_bpm = 0;
    }

    void HrDetector::push(uint32_t tMs, int32_t ir)
    {
        const float x = static_cast<float>(ir);

        // Single-pole DC remover (Steiglitz form):
        //   y[n] = x[n] - x[n-1] + α · y[n-1]
        // Output is AC-coupled; large constant DC drops out completely.
        // First sample seeds the state so we don't ring on startup.
        float y = 0.0f;
        if (!m_dcInit)
        {
            m_prevX = x;
            m_prevY = 0.0f;
            m_dcInit = true;
        }
        else
        {
            y = x - m_prevX + kDcAlpha * m_prevY;
            m_prevX = x;
            m_prevY = y;
        }

        // 4-tap moving-average smoother.
        m_taps[m_tapIdx] = y;
        m_tapIdx = (m_tapIdx + 1) % kFilterTaps;
        if (m_tapCount < kFilterTaps) { ++m_tapCount; }

        float sum = 0.0f;
        for (int i = 0; i < m_tapCount; ++i) { sum += m_taps[i]; }
        const float smoothed = sum / static_cast<float>(m_tapCount);

        // Adaptive envelope tracking. Asymmetric IIR: rises fast,
        // decays slow on the high envelope; opposite on the low. This
        // is the cheap way to estimate "current peak amplitude" without
        // a real Schmitt trigger.
        if (!m_envInit)
        {
            m_envHigh = smoothed;
            m_envLow  = smoothed;
            m_envInit = true;
        }
        else
        {
            if (smoothed > m_envHigh) { m_envHigh = smoothed; }
            else                       { m_envHigh = 0.999f * m_envHigh + 0.001f * smoothed; }

            if (smoothed < m_envLow)   { m_envLow  = smoothed; }
            else                        { m_envLow  = 0.999f * m_envLow  + 0.001f * smoothed; }
        }

        const float threshold =
            m_envLow + kPeakFraction * (m_envHigh - m_envLow);

        // Rising-edge crossing of the adaptive threshold + refractory.
        // We look for samples that cross UP through the threshold —
        // that's the systolic upstroke, the most repeatable feature in
        // a PPG.
        const bool above = (smoothed > threshold);
        const bool risingEdge = above && !m_aboveThresh;
        m_aboveThresh = above;
        m_lastSmoothed = smoothed;

        if (risingEdge)
        {
            const bool refractoryOk =
                (m_lastPeakMs == 0) ||
                ((tMs - m_lastPeakMs) >= kRefractoryMs);

            if (refractoryOk)
            {
                if (m_lastPeakMs != 0)
                {
                    const uint32_t ibi = tMs - m_lastPeakMs;
                    m_ibis[m_ibiIdx] = ibi;
                    m_ibiIdx = (m_ibiIdx + 1) % kIbiHistory;
                    if (m_ibiCount < kIbiHistory) { ++m_ibiCount; }

                    // Need at least two IBIs before we trust the mean.
                    if (m_ibiCount >= 2)
                    {
                        uint64_t total = 0;
                        for (int i = 0; i < m_ibiCount; ++i)
                        {
                            total += m_ibis[i];
                        }
                        const uint32_t meanMs =
                            static_cast<uint32_t>(total / static_cast<uint64_t>(m_ibiCount));
                        if (meanMs > 0)
                        {
                            const uint32_t bpmU32 = 60000u / meanMs;
                            m_bpm = (bpmU32 > 240) ? 240
                                  : static_cast<uint8_t>(bpmU32);
                        }
                    }
                }
                m_lastPeakMs = tMs;
            }
        }
    }
}
