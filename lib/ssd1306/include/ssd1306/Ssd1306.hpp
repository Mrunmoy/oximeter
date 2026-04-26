#pragma once

#include "ssd1306/IBus.hpp"

#include <cstddef>
#include <cstdint>

// Portable SSD1306 128x64 monochrome OLED driver.
//
// The driver holds a 1024-byte frame buffer and addresses the chip
// in horizontal addressing mode (memory addr mode 0x00). All public
// API uses pixel coordinates with origin at the top-left.
//
// The driver only writes; it never reads back. After init(), the
// usual loop is: clear(), draw*(), flush(). flush() is the only
// method that talks to the bus (and during init()).
//
// No platform headers; no STL. Mirrors the lib/max3010x portability
// contract from the project CLAUDE.md.

namespace oxinode::ssd1306
{
    class Ssd1306
    {
    public:
        // Default I²C address; the SA0 pin selects between 0x3C and
        // 0x3D. The 0.96" modules in the OxiNode bring-up bag default
        // to 0x3C with SA0 tied low.
        static constexpr std::uint8_t kDefaultI2cAddr = 0x3C;

        static constexpr int          kWidth         = 128;
        static constexpr int          kHeight        = 64;
        static constexpr int          kPages         = kHeight / 8;
        static constexpr std::size_t  kFramebufBytes =
            static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(kPages);

        explicit Ssd1306(IBus& bus, std::uint8_t devAddr = kDefaultI2cAddr);

        // Soft-init: sends the SSD1306 power-on sequence (clock div,
        // mux ratio, charge-pump enable, segment remap, addressing
        // mode, contrast, display ON). Returns 0 on success or the
        // bus error from the first failed transaction.
        [[nodiscard]] int init();

        // Pushes the entire frame buffer to the chip. Window is set
        // to the full 128x64 each call so callers can be ignorant
        // of cursor state.
        [[nodiscard]] int flush();

        // Frame buffer manipulation — pure RAM, no I/O.
        void clear();
        void drawPixel(int x, int y, bool on);
        void drawText(int x, int y, const char* text);

        // Convenience: render the OxiNode dashboard (header line + HR
        // line + SpO2 line). `version` may be nullptr.
        void drawDashboard(std::uint8_t bpm,
                           std::uint8_t spo2,
                           const char*  version);

        // Test hook so unit tests can assert pixel layout without
        // having to round-trip through the bus.
        [[nodiscard]] const std::uint8_t* framebuffer() const { return m_fb; }

    private:
        // Sends a stream of command bytes (control byte 0x00 then
        // payload), splitting into ≤32-byte chunks so the firmware's
        // platform HAL doesn't have to size huge stack buffers.
        [[nodiscard]] int sendCommands(const std::uint8_t* cmds, std::size_t n);

        IBus&        m_bus;
        std::uint8_t m_devAddr;
        std::uint8_t m_fb[kFramebufBytes] = {};
    };
}
