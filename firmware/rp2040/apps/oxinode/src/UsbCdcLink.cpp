#include "UsbCdcLink.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "pico/stdio.h"
#include "pico/time.h"

// First bring-up uses pico_stdio_usb only (printf → tinyusb internally).
// Direct tud_cdc_n_* access is gated behind a tusb_config.h that the
// pico-sdk 2.2 stdio_usb path owns; mixing both at the app level
// pulls in symbols that aren't declared. Stdio path covers JSON-Lines
// and binary writes (writeRaw uses putchar) — the only thing we lose
// is host-side command parsing (MODE BIN). Re-enable once we ship a
// custom USB descriptor — see TODO in firmware/rp2040/README.md.
#define OXINODE_HAVE_TINYUSB 0

namespace oxinode::rp2040
{
    namespace
    {
        // CDC interface index. pico_stdio_usb defines this in tinyusb
        // configuration; OxiNode uses a single CDC interface (index 0).
        static constexpr std::uint8_t kCdcItf = 0;

        // 1 ms tud_task pump. TinyUSB needs this serviced regularly
        // or USB enumeration / writes will stall.
        static constexpr std::int32_t kUsbPumpPeriodUs = 1000;

        // Repeating-timer instance; owned by the singleton CDC link.
        // Heap-free (the timer SDK stores by value into this struct).
        static repeating_timer_t s_pumpTimer{};

        bool tudPumpCallback(repeating_timer_t* /*rt*/)
        {
#if OXINODE_HAVE_TINYUSB
            tud_task();
#endif
            return true;
        }

        // Best-effort parser: lower-cases s in place up to len.
        void asciiToUpper(char* s, std::size_t len)
        {
            for (std::size_t i = 0; i < len; ++i)
            {
                if (s[i] >= 'a' && s[i] <= 'z')
                {
                    s[i] = static_cast<char>(s[i] - ('a' - 'A'));
                }
            }
        }
    }

    void UsbCdcLink::init()
    {
        if (m_inited)
        {
            return;
        }

        // Servicing tud_task() from a repeating timer keeps USB alive
        // even when core0's main loop is blocked on a long printf or
        // a busy core1. The timer fires from the timer IRQ, which is
        // priority-safe with respect to our GPIO ISR on core1.
        add_repeating_timer_us(-kUsbPumpPeriodUs,
                               &tudPumpCallback,
                               nullptr,
                               &s_pumpTimer);
        m_inited = true;
    }

    void UsbCdcLink::setMode(Mode mode)
    {
        if (mode == m_mode)
        {
            return;
        }
        // Announce the transition in JSON before we flip — once we
        // flip to BIN the host is no longer parsing newlines.
        if (m_mode == Mode::Json)
        {
            std::printf("{\"status\":\"mode\",\"to\":\"%s\"}\n",
                        mode == Mode::Bin ? "bin" : "json");
            stdio_flush();
        }
        m_mode = mode;
    }

    void UsbCdcLink::writeSample(std::uint32_t tMs, std::uint32_t ir, std::uint32_t red,
                                 std::int16_t hr, std::int16_t spo2)
    {
        if (m_mode == Mode::Json)
        {
            // Negative hr/spo2 → still warming up. Emit `null` rather
            // than a sentinel so a JSON consumer doesn't have to know
            // the magic value.
            char hrBuf[8];
            char spo2Buf[8];
            if (hr < 0)
            {
                std::snprintf(hrBuf, sizeof(hrBuf), "null");
            }
            else
            {
                std::snprintf(hrBuf, sizeof(hrBuf), "%d", static_cast<int>(hr));
            }
            if (spo2 < 0)
            {
                std::snprintf(spo2Buf, sizeof(spo2Buf), "null");
            }
            else
            {
                std::snprintf(spo2Buf, sizeof(spo2Buf), "%d", static_cast<int>(spo2));
            }
            std::printf(
                "{\"t\":%lu,\"ir\":%lu,\"red\":%lu,\"hr\":%s,\"spo2\":%s}\n",
                static_cast<unsigned long>(tMs),
                static_cast<unsigned long>(ir),
                static_cast<unsigned long>(red),
                hrBuf,
                spo2Buf);
        }
        else
        {
            // BIN mode: raw little-endian SAMPLE record. The portable
            // Framer (lib/max3010x) handles the on-wire CRC framing
            // for buffered records; this path emits the inner payload
            // directly so we can preserve sample timing without an
            // extra copy. Layout (see docs/PROTOCOL.md SAMPLE frame):
            //   u8  type = 0x01
            //   u8  flags
            //   u32 t_ms     (le)
            //   u32 ir       (le)
            //   u32 red      (le)
            //   i16 hr       (le, -1 = invalid)
            //   i16 spo2     (le, -1 = invalid)
            std::uint8_t buf[1 + 1 + 4 + 4 + 4 + 2 + 2];
            buf[0] = 0x01;
            buf[1] = 0x00;
            std::memcpy(&buf[2],  &tMs, 4);
            std::memcpy(&buf[6],  &ir,  4);
            std::memcpy(&buf[10], &red, 4);
            std::memcpy(&buf[14], &hr,  2);
            std::memcpy(&buf[16], &spo2, 2);
            writeRaw(buf, sizeof(buf));
        }
    }

    void UsbCdcLink::writeStatus(const char* status, const char* reason)
    {
        if (status == nullptr)
        {
            status = "";
        }
        if (reason == nullptr)
        {
            reason = "";
        }
        if (m_mode == Mode::Json)
        {
            std::printf("{\"status\":\"%s\",\"reason\":\"%s\"}\n", status, reason);
        }
        else
        {
            // Type 0x02 = STATUS frame. Length-prefixed C strings.
            const std::size_t sLen = std::strlen(status);
            const std::size_t rLen = std::strlen(reason);
            // Cap to avoid blowing the bounded stack buffer.
            const std::size_t kMax = 96;
            const std::size_t s = sLen > kMax ? kMax : sLen;
            const std::size_t r = rLen > kMax ? kMax : rLen;

            std::uint8_t hdr[4];
            hdr[0] = 0x02;
            hdr[1] = 0x00;
            hdr[2] = static_cast<std::uint8_t>(s);
            hdr[3] = static_cast<std::uint8_t>(r);
            writeRaw(hdr, sizeof(hdr));
            writeRaw(reinterpret_cast<const std::uint8_t*>(status), s);
            writeRaw(reinterpret_cast<const std::uint8_t*>(reason), r);
        }
    }

    void UsbCdcLink::writeAlive(const AliveStats& s)
    {
        if (m_mode == Mode::Json)
        {
            // The line is long but every field has paid for its
            // place — host-side `scripts/burn-in.sh` reads this
            // verbatim. Keep field names short to minimise USB
            // bandwidth.
            std::printf(
                "{\"t\":%lu,\"alive\":1,\"edges\":%lu,\"hr\":%d,\"spo2\":%d,"
                "\"probe\":%d,\"cfg\":%d,\"int1\":%u,\"int2\":%u,\"drain\":%ld,"
                "\"smpl_d\":%lu,\"smpl_c\":%lu,"
                "\"chip_ovf\":%lu,\"pwr_rdy\":%lu,\"alc_ovf\":%lu,\"i2c_err\":%lu,"
                "\"ring_drops\":%lu,\"ring_hwm\":%lu,"
                "\"dsp_us_max\":%lu,\"dsp_overbudget\":%lu,"
                "\"fault_flags\":%lu,\"cfg_crc\":%u}\n",
                static_cast<unsigned long>(s.tMs),
                static_cast<unsigned long>(s.edges),
                static_cast<int>(s.hr),
                static_cast<int>(s.spo2),
                static_cast<int>(s.probeRc),
                static_cast<int>(s.configureRc),
                static_cast<unsigned>(s.int1),
                static_cast<unsigned>(s.int2),
                static_cast<long>(s.lastDrainRc),
                static_cast<unsigned long>(s.samplesDrained),
                static_cast<unsigned long>(s.samplesConsumed),
                static_cast<unsigned long>(s.chipOvf),
                static_cast<unsigned long>(s.pwrRdy),
                static_cast<unsigned long>(s.alcOvf),
                static_cast<unsigned long>(s.i2cErr),
                static_cast<unsigned long>(s.ringDrops),
                static_cast<unsigned long>(s.ringHwm),
                static_cast<unsigned long>(s.dspUsMax),
                static_cast<unsigned long>(s.dspOverbudget),
                static_cast<unsigned long>(s.faultFlags),
                static_cast<unsigned>(s.cfgCrc));
        }
        else
        {
            // Bin mode keeps the original v1 layout — the extended
            // fields land here only when the binary protocol is
            // re-enabled (D-11) and a schema bump is published.
            std::uint8_t buf[1 + 1 + 4 + 4 + 2 + 2];
            buf[0] = 0x03;       // ALIVE
            buf[1] = 0x00;
            std::memcpy(&buf[2],  &s.tMs,   4);
            std::memcpy(&buf[6],  &s.edges, 4);
            std::memcpy(&buf[10], &s.hr,    2);
            std::memcpy(&buf[12], &s.spo2,  2);
            writeRaw(buf, sizeof(buf));
        }
    }

    void UsbCdcLink::pollHostInput()
    {
#if !OXINODE_HAVE_TINYUSB
        // No CDC RX path without tinyusb. The control parser is dead
        // weight here; just return so callers can still call us.
        return;
#else
        // tud_cdc_available is safe to call from any context once
        // tinyusb is up; it returns 0 when nothing is queued.
        while (tud_cdc_n_available(kCdcItf) > 0)
        {
            std::uint8_t b = 0;
            const std::uint32_t got = tud_cdc_n_read(kCdcItf, &b, 1);
            if (got == 0)
            {
                break;
            }

            const char c = static_cast<char>(b);
            if (c == '\n' || c == '\r')
            {
                if (m_lineLen > 0)
                {
                    onLine(m_lineBuf, m_lineLen);
                    m_lineLen = 0;
                }
                continue;
            }

            if (m_lineLen + 1 < kLineBufBytes)
            {
                m_lineBuf[m_lineLen++] = c;
            }
            else
            {
                // Overflow: drop the partial line, resync at next \n.
                m_lineLen = 0;
            }
        }
#endif // OXINODE_HAVE_TINYUSB
    }

    // ── Internals ───────────────────────────────────────────────
    void UsbCdcLink::onLine(const char* line, std::size_t len)
    {
        // Work on a local upper-cased copy. ASCII only — control
        // strings are case-insensitive but always 7-bit.
        char tmp[kLineBufBytes];
        const std::size_t n = (len < kLineBufBytes - 1) ? len : (kLineBufBytes - 1);
        std::memcpy(tmp, line, n);
        tmp[n] = '\0';
        asciiToUpper(tmp, n);

        if (std::strncmp(tmp, "MODE BIN", 8) == 0)
        {
            setMode(Mode::Bin);
        }
        else if (std::strncmp(tmp, "MODE JSON", 9) == 0)
        {
            setMode(Mode::Json);
        }
        else
        {
            // Unknown command. In JSON mode, surface it; in BIN mode
            // stay silent rather than corrupt the binary stream.
            if (m_mode == Mode::Json)
            {
                std::printf("{\"status\":\"err\",\"reason\":\"unknown_cmd\"}\n");
            }
        }
    }

    void UsbCdcLink::writeRaw(const std::uint8_t* data, std::size_t len)
    {
#if !OXINODE_HAVE_TINYUSB
        // No tinyusb: route raw bytes through stdio. picocom can't
        // render them but the firmware still runs end-to-end.
        for (std::size_t i = 0; i < len; ++i)
        {
            std::putchar(static_cast<int>(data[i]));
        }
        std::fflush(stdout);
#else
        // tud_cdc_n_write copies into TinyUSB's TX FIFO. We then have
        // to flush so the host actually sees the bytes (otherwise
        // they'd sit until the FIFO fills or USB IN poll rolls over).
        std::size_t off = 0;
        while (off < len)
        {
            const std::uint32_t avail = tud_cdc_n_write_available(kCdcItf);
            if (avail == 0)
            {
                // Pump the USB stack so the host can drain. Falls
                // back gracefully if the host isn't reading; we drop
                // bytes only when the FIFO has been full for a long
                // time, which we treat as benign data loss.
                tud_task();
                if (tud_cdc_n_write_available(kCdcItf) == 0)
                {
                    break;
                }
                continue;
            }
            const std::size_t chunk =
                ((len - off) < avail) ? (len - off) : avail;
            (void)tud_cdc_n_write(kCdcItf, data + off, static_cast<std::uint32_t>(chunk));
            off += chunk;
        }
        (void)tud_cdc_n_write_flush(kCdcItf);
#endif
    }
}
