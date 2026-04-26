#include "max3010x/HrDetector.hpp"

namespace oxinode::max3010x
{
    void HrDetector::reset()
    {
        for (int i = 0; i < kBufferSize; ++i) { m_buf[i] = 0; m_work[i] = 0; }
        for (int i = 0; i < kMaxPeaks; ++i)   { m_locs[i] = 0; }
        for (int i = 0; i < kBpmMedianN; ++i) { m_bpmHistory[i] = 0; }
        m_filled = 0;
        m_writeIdx = 0;
        m_sinceRecompute = 0;
        m_bpmHistoryHead = 0;
        m_bpm = 0;
    }

    uint8_t HrDetector::medianOf3(uint8_t a, uint8_t b, uint8_t c)
    {
        // Branchless median via pairwise max/min: median = max(min(a,b),
        // min(max(a,b), c)). Cheap and avoids a tiny insertion sort.
        const uint8_t lo = (a < b) ? a : b;
        const uint8_t hi = (a < b) ? b : a;
        const uint8_t midHi = (hi < c) ? hi : c;
        return (lo > midHi) ? lo : midHi;
    }

    uint8_t HrDetector::medianOf5(uint8_t a, uint8_t b, uint8_t c,
                                  uint8_t d, uint8_t e)
    {
        // 5-sample insertion sort then pick index 2. The compiler
        // unrolls this into a fixed dependency chain on Cortex-M0+
        // (no branch-mispredict cost) and the whole call inlines
        // away in the only place that matters (`pushOutput`).
        uint8_t s[5] = {a, b, c, d, e};
        for (int i = 1; i < 5; ++i)
        {
            const uint8_t x = s[i];
            int j = i;
            while (j > 0 && s[j - 1] > x)
            {
                s[j] = s[j - 1];
                --j;
            }
            s[j] = x;
        }
        return s[2];
    }

    void HrDetector::pushOutput(uint8_t bpm)
    {
        m_bpmHistory[m_bpmHistoryHead] = bpm;
        m_bpmHistoryHead = (m_bpmHistoryHead + 1) % kBpmMedianN;
        // kBpmMedianN is the static buffer width — currently 5.
        // The static_assert below is a maintenance aid: if a future
        // change tweaks kBpmMedianN, this fails the build and the
        // author has to either update both sites or generalise
        // pushOutput to call a length-N median.
        static_assert(kBpmMedianN == 5,
                      "pushOutput hard-codes a width-5 median; update if "
                      "kBpmMedianN changes (or rewire to a generic medianOfN)");
        m_bpm = medianOf5(m_bpmHistory[0], m_bpmHistory[1], m_bpmHistory[2],
                          m_bpmHistory[3], m_bpmHistory[4]);
    }

    void HrDetector::push(uint32_t /*tMs*/, int32_t ir)
    {
        m_buf[m_writeIdx] = ir;
        m_writeIdx = (m_writeIdx + 1) % kBufferSize;
        if (m_filled < kBufferSize) { ++m_filled; }

        ++m_sinceRecompute;
        if (m_filled == kBufferSize &&
            m_sinceRecompute >= kRecomputeEvery)
        {
            m_sinceRecompute = 0;
            recompute();
        }
    }

    void HrDetector::recompute()
    {
        // Linearise the ring buffer into m_work, oldest-first. The
        // oldest sample sits at m_writeIdx (because we just wrapped).
        const int n = kBufferSize;
        for (int k = 0; k < n; ++k)
        {
            m_work[k] = m_buf[(m_writeIdx + k) % n];
        }

        // Step 1 — buffer mean.
        int64_t sum = 0;
        for (int k = 0; k < n; ++k) { sum += m_work[k]; }
        const int32_t mean = static_cast<int32_t>(sum / n);

        // Step 2 — subtract DC mean and invert. Inversion turns the
        // valley-shaped systolic upstrokes into peaks so we can use a
        // peak-finder (Maxim's stated rationale, algorithm.cpp:111).
        for (int k = 0; k < n; ++k)
        {
            m_work[k] = -1 * (m_work[k] - mean);
        }

        // Step 3 — 4-tap moving-average smoother. Maxim's reference
        // (algorithm.cpp:115) only smooths the first (n - kMaSize)
        // slots and leaves the trailing 4 raw, which produces spurious
        // peaks at the buffer edge for slow signals (≤45 BPM, where
        // there are only 2-3 real peaks per buffer and one spurious
        // peak dominates the mean-interval calc). We follow Maxim's
        // forward-MA exactly, then **zero the trailing kMaSize slots**
        // so find_peaks treats them as below kMinPeakHeight. This
        // costs ~0.16 s of usable signal per 4 s buffer; fast signals
        // are unaffected since they have ≥6 real peaks.
        for (int k = 0; k < n - kMaSize; ++k)
        {
            m_work[k] = (m_work[k] + m_work[k + 1] +
                         m_work[k + 2] + m_work[k + 3]) / 4;
        }
        for (int k = n - kMaSize; k < n; ++k) { m_work[k] = 0; }

        // Step 4 — adaptive threshold. Maxim's reference computes the
        // mean of the demeaned-and-inverted signal and clamps it to
        // [30, 60]. The mean is ≈ 0 after demeaning, so the clamp
        // floor of 30 is what actually applies on a clean trace; we
        // skip the dead arithmetic and use the floor directly.
        const int32_t threshold = kMinPeakHeight;

        // Step 5 — find peaks.
        const int npks = findPeaks(m_locs, m_work, n,
                                   threshold, kMinPeakDistance);

        // Step 6 — BPM from mean inter-peak interval.
        if (npks >= 2)
        {
            int32_t intervalSum = 0;
            for (int k = 1; k < npks; ++k)
            {
                intervalSum += (m_locs[k] - m_locs[k - 1]);
            }
            const int32_t meanInterval = intervalSum / (npks - 1);
            if (meanInterval > 0)
            {
                const uint32_t bpm =
                    static_cast<uint32_t>((kFs * 60) / meanInterval);
                if (bpm >= kBpmMin && bpm <= kBpmMax)
                {
                    pushOutput(static_cast<uint8_t>(bpm));
                    return;
                }
            }
        }
        // No usable peaks — push the "invalid" sentinel into the
        // median ring so a sustained loss of signal (finger removed)
        // fades to 0 over kBpmMedianN cycles.
        pushOutput(0);
    }

    // Direct port of algorithm.cpp::maxim_find_peaks:
    //   1. Walk the signal and collect every local maximum that is
    //      strictly above minHeight (handling flat-top peaks by
    //      taking the leftmost sample of the plateau).
    //   2. Sort indices by amplitude descending, then greedily keep
    //      the largest while removing any peaks within minDistance
    //      of an already-kept one.
    //   3. Re-sort kept indices ascending so the BPM math sees them
    //      in time order.
    int HrDetector::findPeaks(int32_t* outLocs,
                              const int32_t* signal,
                              int n,
                              int32_t minHeight,
                              int minDistance)
    {
        int npks = 0;

        // ── 1. peaks-above-min-height ───────────────────────────────
        int i = 1;
        while (i < n - 1 && npks < kMaxPeaks)
        {
            if (signal[i] > minHeight && signal[i] > signal[i - 1])
            {
                int width = 1;
                while (i + width < n &&
                       signal[i] == signal[i + width])
                {
                    ++width;
                }
                if (i + width < n && signal[i] > signal[i + width])
                {
                    outLocs[npks++] = i;
                    i += width + 1;
                }
                else
                {
                    i += width;
                }
            }
            else
            {
                ++i;
            }
        }

        // ── 2. remove-close-peaks ───────────────────────────────────
        // Sort outLocs by signal[loc] descending (insertion sort).
        for (int a = 1; a < npks; ++a)
        {
            const int32_t key = outLocs[a];
            int b = a;
            while (b > 0 && signal[key] > signal[outLocs[b - 1]])
            {
                outLocs[b] = outLocs[b - 1];
                --b;
            }
            outLocs[b] = key;
        }
        // Greedily keep the tallest, drop anything within minDistance.
        // Pass i = -1 corresponds to the "lag-zero" sentinel in the
        // Maxim reference — the tallest peak is always kept.
        int kept = 0;
        for (int a = -1; a < npks; ++a)
        {
            const int oldNpks = npks;
            const int anchor = (a == -1) ? -1 : outLocs[a];
            kept = a + 1;
            for (int b = a + 1; b < oldNpks; ++b)
            {
                const int dist = outLocs[b] - anchor;
                if (dist > minDistance || dist < -minDistance)
                {
                    outLocs[kept++] = outLocs[b];
                }
            }
            npks = kept;
        }

        // ── 3. resort ascending ─────────────────────────────────────
        for (int a = 1; a < npks; ++a)
        {
            const int32_t key = outLocs[a];
            int b = a;
            while (b > 0 && key < outLocs[b - 1])
            {
                outLocs[b] = outLocs[b - 1];
                --b;
            }
            outLocs[b] = key;
        }

        return npks;
    }
}
