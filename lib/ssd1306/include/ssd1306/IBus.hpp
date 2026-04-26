#pragma once

#include <cstddef>
#include <cstdint>

// Generic write-only I²C bus contract for the SSD1306 driver.
//
// The OLED is a fire-and-forget peripheral: firmware never reads the
// chip back, so the contract is intentionally narrower than the
// MAX30102 driver's `max3010x::IHal`. Keeping a separate, smaller
// interface avoids any cross-driver coupling and makes the host fake
// for OLED tests trivial.
//
// Negative return values are errno-style failures; 0 means success.

namespace oxinode::ssd1306
{
    class IBus
    {
    public:
        virtual ~IBus() = default;

        // START, addr+W, payload, STOP. The first byte of `data` for
        // the SSD1306 is always a control byte (0x00 = command stream,
        // 0x40 = data stream); the driver enforces that convention.
        [[nodiscard]] virtual int write(std::uint8_t devAddr,
                                        const std::uint8_t* data,
                                        std::size_t len) = 0;
    };
}
