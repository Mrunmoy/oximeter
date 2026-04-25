#pragma once

#include <cstdint>

// ── Waveshare RP2040-Zero pin map ────────────────────────────────
// Authoritative source: docs/HARDWARE.md. The RP2040-Zero is a
// vanilla RP2040 in a small castellated module; the silkscreen
// pads on the board edge are labelled GPx where x is the GPIO
// number (no functional remap), so the pad name and the GPIO
// number coincide.
//
// MAX30102 INT is open-drain active-low. Internal pull-up is
// enabled in PicoIntPin to avoid spurious edges.

namespace oxinode::board
{
    // ── I²C0 — MAX30102 ────────────────────────────────────────
    static constexpr unsigned int kPinSda = 4;   // silkscreen pad "GP4" → MAX30102 SDA
    static constexpr unsigned int kPinScl = 5;   // silkscreen pad "GP5" → MAX30102 SCL

    // ── INT line ──────────────────────────────────────────────
    static constexpr unsigned int kPinInt = 6;   // silkscreen pad "GP6" → MAX30102 INT

    // ── I²C bus configuration ─────────────────────────────────
    // 100 kHz on v1; the MAX30102 datasheet rates the bus to
    // 400 kHz (fast-mode). Stay at 100 kHz until bring-up has
    // verified clean edges on a scope.
    static constexpr unsigned int kI2cFreqHz = 100u * 1000u;

    // ── MAX30102 7-bit I²C address ────────────────────────────
    static constexpr std::uint8_t kMax3010xI2cAddr = 0x57;
}
