#pragma once

#include <cstdint>

// 5x7 monospace ASCII font, printable range 0x20..0x7E (95 glyphs).
//
// Layout: each glyph is 5 bytes, one byte per pixel column. Within a
// byte, bit 0 is the top row and bit 6 is the bottom row; bit 7 is
// unused (the font is 7 px tall). The driver pairs this with a
// 1-pixel inter-character gap so the cell width is 6 px.
//
// Source: a well-known public-domain 5x7 font (the same shape is
// shipped with countless Arduino / MicroPython libraries). The
// pattern is reproduced here so the lib has no external font
// dependency.

namespace oxinode::ssd1306
{
    inline constexpr int kFontGlyphWidth   = 5;
    inline constexpr int kFontGlyphSpacing = 1;
    inline constexpr int kFontCellWidth    = kFontGlyphWidth + kFontGlyphSpacing;
    inline constexpr int kFontHeight       = 7;
    inline constexpr int kFontGlyphCount   = 95;

    extern const std::uint8_t kFont5x7[kFontGlyphCount * kFontGlyphWidth];
}
