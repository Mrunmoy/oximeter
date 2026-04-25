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

        struct Config
        {
            uint8_t          devAddr   = reg::kI2cAddr;
            Mode             mode      = Mode::Spo2;
            SampleAveraging  avg       = SampleAveraging::AVG_4;
            SampleRate       rate      = SampleRate::SR_100;
            PulseWidth       pulseWidth = PulseWidth::PW_411_18BIT;
            AdcRange         adcRange   = AdcRange::RANGE_4096;
            // Raw register codes — datasheet table 8 gives ~0.2 mA per
            // count; 0x24 ≈ 7 mA per LED is a safe default for a
            // pressed-finger SpO2 reading on a GY-MAX30102 breakout.
            uint8_t          redLedPa  = 0x24;
            uint8_t          irLedPa   = 0x24;
            // FIFO almost-full triggers when (32 - threshold) entries
            // remain unread — i.e. threshold == 17 means "fire IRQ when
            // 15 unread entries are buffered". 0x0F is the chip default.
            uint8_t          fifoAlmostFullThreshold = 0x0F;
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
        // Burst-read scratch. Sized for the worst case (SpO2 mode,
        // 32 entries × 6 bytes = 192 B). Statically allocated — caller
        // never sees it.
        uint8_t          m_burst[kFifoBurstBytes] = {};
    };
}
