#include "ssd1306/Ssd1306.hpp"
#include "ssd1306/Font5x7.hpp"

#include <cstring>

// SSD1306 driver implementation.
//
// The chip is configured for horizontal addressing mode (memory addr
// 0x00): writes auto-advance column-then-page across the full 128x64
// area. flush() always re-asserts the column/page window so callers
// can be ignorant of cursor state — at 1 Hz update rate the few extra
// command bytes are negligible.
//
// All bus writes follow the SSD1306 control-byte convention:
//   - 0x00 control byte → command stream
//   - 0x40 control byte → data stream (frame buffer)
// The driver chunks bus writes to ≤ 33 bytes (1 control + 32 payload)
// so the pico-sdk-side I²C glue does not need a large stack scratch.

namespace oxinode::ssd1306
{
    namespace
    {
        // ── Control bytes ──────────────────────────────────────
        constexpr std::uint8_t kCtrlCmd  = 0x00;
        constexpr std::uint8_t kCtrlData = 0x40;

        // ── SSD1306 command opcodes (subset used by this driver) ──
        constexpr std::uint8_t kCmdSetContrast      = 0x81;
        constexpr std::uint8_t kCmdEntireDispResume = 0xA4;
        constexpr std::uint8_t kCmdSetNormalDisp    = 0xA6;
        constexpr std::uint8_t kCmdDisplayOff       = 0xAE;
        constexpr std::uint8_t kCmdDisplayOn        = 0xAF;
        constexpr std::uint8_t kCmdSetDisplayOffset = 0xD3;
        constexpr std::uint8_t kCmdSetClockDiv      = 0xD5;
        constexpr std::uint8_t kCmdSetPrecharge     = 0xD9;
        constexpr std::uint8_t kCmdSetComPins       = 0xDA;
        constexpr std::uint8_t kCmdSetVcomDeselect  = 0xDB;
        constexpr std::uint8_t kCmdChargePump       = 0x8D;
        constexpr std::uint8_t kCmdMemoryAddrMode   = 0x20;
        constexpr std::uint8_t kCmdSetColumnAddr    = 0x21;
        constexpr std::uint8_t kCmdSetPageAddr      = 0x22;
        constexpr std::uint8_t kCmdSetMuxRatio      = 0xA8;
        constexpr std::uint8_t kCmdSetStartLine     = 0x40;  // 0x40..0x7F
        constexpr std::uint8_t kCmdSegRemap         = 0xA0;  // 0xA0|seg-remap-bit
        constexpr std::uint8_t kCmdComScanDir       = 0xC0;  // 0xC0|scan-dir-bit
        constexpr std::uint8_t kCmdDeactivateScroll = 0x2E;

        // Standard 128x64 SSD1306 init sequence — same shape as the
        // Adafruit reference, with the charge-pump enabled (0x8D 0x14)
        // because the OxiNode wiring runs the panel from the 3V3
        // rail rather than an external 7-9 V VCC supply.
        constexpr std::uint8_t kInitSeq[] = {
            kCmdDisplayOff,
            kCmdSetClockDiv,      0x80,         // suggested ratio 0x80
            kCmdSetMuxRatio,      0x3F,         // 64 - 1
            kCmdSetDisplayOffset, 0x00,
            static_cast<std::uint8_t>(kCmdSetStartLine | 0x00),
            kCmdChargePump,       0x14,         // enable internal charge pump
            kCmdMemoryAddrMode,   0x00,         // horizontal addressing
            static_cast<std::uint8_t>(kCmdSegRemap   | 0x01), // 0xA1
            static_cast<std::uint8_t>(kCmdComScanDir | 0x08), // 0xC8
            kCmdSetComPins,       0x12,         // alt COM pins, no L/R remap
            kCmdSetContrast,      0xCF,
            kCmdSetPrecharge,     0xF1,         // for charge-pump operation
            kCmdSetVcomDeselect,  0x40,
            kCmdEntireDispResume,
            kCmdSetNormalDisp,
            kCmdDeactivateScroll,
            kCmdDisplayOn,
        };

        // Bus-write chunk size for both command and data streams.
        // Sized so that 1 control byte + kChunkMax data bytes fits
        // comfortably inside the firmware-side i2c stack scratch
        // (PicoI2cHal uses 64 bytes; we leave headroom).
        constexpr std::size_t kChunkMax = 32;
    }

    Ssd1306::Ssd1306(IBus& bus, std::uint8_t devAddr)
        : m_bus(bus)
        , m_devAddr(devAddr)
    {
    }

    int Ssd1306::sendCommands(const std::uint8_t* cmds, std::size_t n)
    {
        std::size_t offset = 0;
        while (offset < n)
        {
            const std::size_t remaining = n - offset;
            const std::size_t chunk     = (remaining > kChunkMax) ? kChunkMax : remaining;

            std::uint8_t buf[kChunkMax + 1];
            buf[0] = kCtrlCmd;
            std::memcpy(&buf[1], cmds + offset, chunk);

            const int rc = m_bus.write(m_devAddr, buf, chunk + 1);
            if (rc != 0)
            {
                return rc;
            }
            offset += chunk;
        }
        return 0;
    }

    int Ssd1306::init()
    {
        return sendCommands(kInitSeq, sizeof(kInitSeq));
    }

    void Ssd1306::clear()
    {
        std::memset(m_fb, 0, sizeof(m_fb));
    }

    void Ssd1306::drawPixel(int x, int y, bool on)
    {
        if (x < 0 || x >= kWidth || y < 0 || y >= kHeight)
        {
            return;
        }
        const int          page = y >> 3;
        const int          bit  = y & 7;
        const std::size_t  idx  = static_cast<std::size_t>(page) * static_cast<std::size_t>(kWidth)
                                  + static_cast<std::size_t>(x);
        const std::uint8_t mask = static_cast<std::uint8_t>(1u << bit);

        if (on)
        {
            m_fb[idx] = static_cast<std::uint8_t>(m_fb[idx] | mask);
        }
        else
        {
            m_fb[idx] = static_cast<std::uint8_t>(m_fb[idx] & ~mask);
        }
    }

    void Ssd1306::drawText(int x, int y, const char* text)
    {
        if (text == nullptr)
        {
            return;
        }
        int cx = x;
        while (*text != '\0')
        {
            unsigned char c = static_cast<unsigned char>(*text++);
            // Map non-printable / out-of-range chars to '?' so the
            // glyph index stays in bounds.
            if (c < 0x20 || c > 0x7E)
            {
                c = '?';
            }
            const std::uint8_t* glyph =
                &kFont5x7[static_cast<std::size_t>(c - 0x20) * static_cast<std::size_t>(kFontGlyphWidth)];

            for (int col = 0; col < kFontGlyphWidth; ++col)
            {
                const std::uint8_t bits = glyph[col];
                for (int row = 0; row < kFontHeight; ++row)
                {
                    const bool on = ((bits >> row) & 0x01u) != 0u;
                    drawPixel(cx + col, y + row, on);
                }
            }
            cx += kFontCellWidth;
            if (cx >= kWidth)
            {
                break;
            }
        }
    }

    int Ssd1306::flush()
    {
        // Re-assert the full-screen window every flush. Three two-byte
        // commands (0x21 col-addr, 0x22 page-addr) plus payload.
        const std::uint8_t window[] = {
            kCmdSetColumnAddr, 0x00, 0x7F,
            kCmdSetPageAddr,   0x00, static_cast<std::uint8_t>(kPages - 1),
        };
        int rc = sendCommands(window, sizeof(window));
        if (rc != 0)
        {
            return rc;
        }

        std::size_t offset = 0;
        while (offset < kFramebufBytes)
        {
            const std::size_t remaining = kFramebufBytes - offset;
            const std::size_t chunk     = (remaining > kChunkMax) ? kChunkMax : remaining;

            std::uint8_t buf[kChunkMax + 1];
            buf[0] = kCtrlData;
            std::memcpy(&buf[1], &m_fb[offset], chunk);

            rc = m_bus.write(m_devAddr, buf, chunk + 1);
            if (rc != 0)
            {
                return rc;
            }
            offset += chunk;
        }
        return 0;
    }

    namespace
    {
        // Tiny helper: write a 0..999 unsigned int as a 3-char,
        // space-padded decimal at out[0..2]. Avoids snprintf so the
        // driver stays free of <cstdio> on embedded.
        void formatU8(std::uint8_t v, char out[3])
        {
            const std::uint8_t hundreds = static_cast<std::uint8_t>(v / 100u);
            const std::uint8_t tens     = static_cast<std::uint8_t>((v / 10u) % 10u);
            const std::uint8_t ones     = static_cast<std::uint8_t>(v % 10u);

            // Suppress leading zeros so "75" prints as " 75" not "075".
            out[0] = (hundreds != 0u) ? static_cast<char>('0' + hundreds) : ' ';
            out[1] = (hundreds != 0u || tens != 0u) ? static_cast<char>('0' + tens) : ' ';
            out[2] = static_cast<char>('0' + ones);
        }
    }

    void Ssd1306::drawDashboard(std::uint8_t bpm,
                                std::uint8_t spo2,
                                const char*  version)
    {
        clear();

        // Page 0 (rows 0..6): "OxiNode" + version.
        drawText(0, 0, "OxiNode");
        if (version != nullptr)
        {
            // Right-aligned to leave room for ~4-char version like "v1.0".
            drawText(98, 0, version);
        }

        // Page 2 (rows 16..22): HR.
        char line[16];
        line[0] = 'H'; line[1] = 'R'; line[2] = ' '; line[3] = ' '; line[4] = ':'; line[5] = ' ';
        if (bpm == 0u)
        {
            line[6] = '-'; line[7] = '-'; line[8] = '-';
        }
        else
        {
            char buf[3];
            formatU8(bpm, buf);
            line[6] = buf[0]; line[7] = buf[1]; line[8] = buf[2];
        }
        line[9]  = ' ';
        line[10] = 'b';
        line[11] = 'p';
        line[12] = 'm';
        line[13] = '\0';
        drawText(0, 16, line);

        // Page 4 (rows 32..38): SpO2.
        line[0] = 'S'; line[1] = 'p'; line[2] = 'O'; line[3] = '2'; line[4] = ':'; line[5] = ' ';
        if (spo2 == 0u)
        {
            line[6] = '-'; line[7] = '-'; line[8] = '-';
        }
        else
        {
            char buf[3];
            formatU8(spo2, buf);
            line[6] = buf[0]; line[7] = buf[1]; line[8] = buf[2];
        }
        line[9]  = ' ';
        line[10] = '%';
        line[11] = '\0';
        drawText(0, 32, line);
    }
}
