#pragma once

#include <cstddef>
#include <cstdint>

namespace oxinode::rp2040
{
    // ── USB-CDC link layer ──────────────────────────────────────
    //
    // Owns the CDC interface on top of pico-sdk's stdio-USB / TinyUSB.
    //
    //   - Boot mode  : Json — newline-delimited JSON ("JSON-Lines"),
    //                  printf-friendly, eyeballable in picocom.
    //   - Bin  mode  : length-prefixed CRC-16/CCITT frames produced
    //                  by oxinode::max3010x::Framer (see lib/).
    //
    // The host switches modes by sending the ASCII line "MODE BIN\n"
    // or "MODE JSON\n". Parsing happens in pollHostInput(), which the
    // main loop is expected to call frequently (every ~1 ms — wired
    // to the same repeating timer that drives tud_task()).
    class UsbCdcLink
    {
    public:
        enum class Mode : std::uint8_t
        {
            Json = 0,
            Bin  = 1,
        };

        UsbCdcLink()                             = default;
        ~UsbCdcLink()                            = default;
        UsbCdcLink(const UsbCdcLink&)            = delete;
        UsbCdcLink& operator=(const UsbCdcLink&) = delete;
        UsbCdcLink(UsbCdcLink&&)                 = delete;
        UsbCdcLink& operator=(UsbCdcLink&&)      = delete;

        // ── Lifecycle ───────────────────────────────────────────
        // Wires the periodic tud_task() pump (1 ms repeating timer)
        // and clears the line buffer. Safe to call multiple times.
        void init();

        // ── Mode ────────────────────────────────────────────────
        void                    setMode(Mode mode);
        [[nodiscard]] Mode      mode() const { return m_mode; }

        // ── Outgoing — sample frame ─────────────────────────────
        // In JSON mode prints
        //   {"t":<ms>,"ir":<u32>,"red":<u32>,"hr":<i16>,"spo2":<i16>}
        // In BIN mode emits a SAMPLE frame via the driver Framer.
        // hr / spo2 < 0 indicate "not yet computed".
        void writeSample(std::uint32_t tMs, std::uint32_t ir, std::uint32_t red,
                         std::int16_t hr, std::int16_t spo2);

        // ── Outgoing — status frame ─────────────────────────────
        // Free-form one-shot status with structured fields.
        void writeStatus(const char* status, const char* reason);

        // ── Outgoing — alive heartbeat (1 Hz) ───────────────────
        // Diagnostic-rich: surfaces probe / configure return codes,
        // the latest INTR_STATUS_{1,2}, every observability counter
        // (Stage A driver stats, Stage B ring stats, Stage C liveness
        // flags). Counters are monotonic u32 — the host computes
        // deltas if it wants a per-second view.
        struct AliveStats
        {
            std::uint32_t tMs;
            std::uint32_t edges;
            std::int16_t  hr;
            std::int16_t  spo2;
            std::int8_t   probeRc;
            std::int8_t   configureRc;
            std::uint8_t  int1;
            std::uint8_t  int2;
            std::int32_t  lastDrainRc;

            // Stage A — driver-level
            std::uint32_t samplesDrained;
            std::uint32_t chipOvf;
            std::uint32_t pwrRdy;
            std::uint32_t alcOvf;
            std::uint32_t i2cErr;

            // Stage B — ring + consumer
            std::uint32_t samplesConsumed;
            std::uint32_t ringDrops;
            std::uint32_t ringHwm;
            std::uint32_t dspUsMax;
            std::uint32_t dspOverbudget;

            // Stage C — soft liveness summary
            std::uint32_t faultFlags;
        };

        void writeAlive(const AliveStats& s);

        // ── Incoming — control parser ───────────────────────────
        // Drains any bytes available on CDC RX and parses complete
        // lines for control commands. Non-blocking; safe to call
        // from core0's tight loop.
        void pollHostInput();

    private:
        // Bounded line buffer for the control parser. ASCII control
        // commands are short ("MODE BIN\n", "MODE JSON\n", ...).
        // Anything longer than this is dropped on the floor.
        static constexpr std::size_t kLineBufBytes = 64;

        void onLine(const char* line, std::size_t len);

        // Raw write helper — bypasses stdio so BIN mode doesn't
        // collide with printf state. Falls through to printf in
        // JSON mode for terminal behaviour and \n translation.
        void writeRaw(const std::uint8_t* data, std::size_t len);

        Mode         m_mode{Mode::Json};
        char         m_lineBuf[kLineBufBytes]{};
        std::size_t  m_lineLen{0};
        bool         m_inited{false};
    };
}
