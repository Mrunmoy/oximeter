#pragma once

#include "max3010x/Hal.hpp"
#include "max3010x/HrDetector.hpp"
#include "max3010x/Registers.hpp"
#include "max3010x/SampleObserver.hpp"
#include "max3010x/Spo2Algo.hpp"

#include <cstdint>

// MAX30102 driver. The class deliberately holds no platform state — all
// I/O goes through `IHal&`. Construction does *not* talk to the chip;
// callers explicitly probe()/configure() so unit tests can run the
// constructor with a stub HAL.

namespace oxinode::max3010x
{
    class Max30102
    {
    public:
        // Maximum number of observers wired in at once. 4 is plenty for
        // the typical "raw logger + JSON formatter + binary formatter
        // + WS broadcaster" topology, and avoids `std::vector`.
        static constexpr int kMaxObservers = 4;

        // FIFO is 32 entries × up to 6 bytes (RED+IR in SpO2 mode); the
        // driver drains it whole in one I²C burst, so this is the
        // largest read we'll ever issue.
        static constexpr int kFifoBurstBytes = reg::kFifoDepth * reg::kBytesPerEntrySpo2;

        // Telemetry / observability surface. Counters are *monotonic*
        // — they only ever increase. Callers compute deltas if they
        // want a per-window view. Reset to zero only when the driver
        // is reconstructed (or via reset(), which is what configure()
        // does on a brownout-recovery path). The convention follows
        // the design discussion in `docs/DESIGN.md` D-15: never reset
        // counters on read, lest the host miss a brief recall event.
        struct Stats
        {
            uint32_t samplesDrained = 0;   // total samples decoded across all calls
            uint32_t chipOvfTotal   = 0;   // sum of OVF_COUNTER values seen
            uint32_t pwrRdyEvents   = 0;   // brownout-recovery count
            uint32_t alcOvfEvents   = 0;   // ambient-light-cancellation overflow
            uint32_t i2cErrTotal    = 0;   // I²C transactions that failed
            uint8_t  lastInt1       = 0;   // last-read INTR_STATUS_1
            uint8_t  lastInt2       = 0;   // last-read INTR_STATUS_2
        };

        struct Config
        {
            uint8_t          devAddr   = reg::kI2cAddr;
            Mode             mode      = Mode::Spo2;
            // SR=100 Hz × SMP_AVE=4 → 25 Hz output, the rate Maxim's
            // HR reference algorithm (UG6409 p.29-30) is tuned for.
            SampleAveraging  avg       = SampleAveraging::AVG_4;
            SampleRate       rate      = SampleRate::SR_100;
            PulseWidth       pulseWidth = PulseWidth::PW_411_18BIT;
            AdcRange         adcRange   = AdcRange::RANGE_4096;
            // 0x3F ≈ 12.5 mA peak (LSB ≈ 0.2 mA per datasheet Table 8).
            // On the GY-MAX30102 breakout pressed against a fingertip,
            // this lands DC at ≈240 K (IR) and ≈207 K (RED) — well above
            // AN6845's 150 K finger-floor (p.9). DC sits at ~91 % FS,
            // *above* UG6409 p.19's "¼ – ¾ FS" SpO2 sweet-spot, but in
            // practice peak DC stays ~7 % below the 18-bit ADC ceiling
            // and the larger AC swing (~12 K vs 8 K @ 0x30) materially
            // improves HR find_peaks SNR. The UG6409 ¾-FS ceiling is a
            // production-skin-tone-spread headroom rule that does not
            // bind for benchtop bring-up. Verified empirically: 0x30
            // hit the doctrinal target but HR scattered 48–166 BPM,
            // while 0x3F locks cleanly to a single resting BPM.
            uint8_t          redLedPa  = 0x3F;
            uint8_t          irLedPa   = 0x3F;
            // FIFO_A_FULL trigger threshold (FIFO_CONFIG[3:0]). The
            // chip fires INT.A_FULL when (32 − raw value) unread
            // entries are queued; encoding details and the
            // `fifoAFullOnUnread<N>()` helper live next to the enum
            // in `Registers.hpp`. Default is the chip's own POR value
            // (17 unread). Strongly-typed so an out-of-range raw u8
            // can no longer silently truncate to 4 bits.
            FifoAFull        fifoAFull = FifoAFull::Unread17;
            // Roll over rather than freeze when the FIFO is full —
            // we'd rather lose old samples than block the chip waiting.
            bool             fifoRollover = true;
        };

        explicit Max30102(IHal& hal);

        // Read PART_ID and verify it equals 0x15. Returns 0 on success,
        // -1 on I²C error, -2 on wrong-id mismatch.
        [[nodiscard]] int probe();

        // Sequentially writes the register block per `cfg`. Issues a
        // soft-reset first to start from a known state.
        [[nodiscard]] int configure(const Config& cfg);

        // MODE_CONFIG.RESET — clears all configuration and FIFO.
        [[nodiscard]] int reset();

        // MODE_CONFIG.SHDN — gates the analog blocks; LEDs go dark.
        [[nodiscard]] int shutdown();
        [[nodiscard]] int wakeup();

        // Service routine: read INTR_STATUS_{1,2}, drain the FIFO if
        // PPG_RDY or A_FULL is set, fan samples to observers and DSP.
        // Returns the number of samples drained, or negative on error.
        [[nodiscard]] int handleInterrupt();

        // One-shot die temperature read. Blocks for ~30 ms while the
        // chip integrates. Caller should not invoke this from an ISR.
        [[nodiscard]] int readTemperatureC(float& out);

        // Subscribe an observer. Silently ignored once kMaxObservers is
        // reached — embedded code can't dynamically grow.
        void addObserver(ISampleObserver* obs);

        // Latest BPM / SpO2 (0 if not yet valid).
        [[nodiscard]] uint8_t bpm() const  { return m_hr.bpm(); }
        [[nodiscard]] uint8_t spo2() const { return m_spo2.spo2(); }

        // Snapshot of internal observability counters. Cheap (8 words),
        // safe to call from any context. See `Stats` for semantics.
        [[nodiscard]] Stats stats() const { return m_stats; }

    private:
        // Decodes one 6-byte SpO2 FIFO entry (RED first, then IR). Each
        // 3-byte channel is MSB-first; only the low 18 bits are valid.
        static void decodeSpo2Entry(const uint8_t* p,
                                    uint32_t& red, uint32_t& ir);

        // Decodes one 3-byte HR-only entry (IR only).
        static void decodeHrEntry(const uint8_t* p, uint32_t& ir);

        void notifySample(uint32_t tMs, uint32_t ir, uint32_t red);
        void notifyHrSpo2(uint32_t tMs);

    private:
        IHal&            m_hal;
        Config           m_cfg{};
        bool             m_configured = false;
        ISampleObserver* m_observers[kMaxObservers] = {};
        int              m_observerCount = 0;
        HrDetector       m_hr{};
        Spo2Algo         m_spo2{};
        Stats            m_stats{};
        // Burst-read scratch. Sized for the worst case (SpO2 mode,
        // 32 entries × 6 bytes = 192 B). Statically allocated — caller
        // never sees it.
        uint8_t          m_burst[kFifoBurstBytes] = {};
    };
}
