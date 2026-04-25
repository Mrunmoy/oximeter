#pragma once

#include <cstddef>
#include <cstdint>

// Driver-core HAL contract. The MAX30102 driver speaks the world only
// through this interface — keeping `lib/` portable across RP2040, ESP32-S3,
// and the host gtest fake. Negative return values are errno-style failures;
// 0 means success. There is deliberately no I/O on the C++ exception path.

namespace oxinode::max3010x
{
    class IHal
    {
    public:
        virtual ~IHal() = default;

        // Burst write of `len` bytes starting at `reg` on slave `devAddr`.
        // Implementations issue a START, addr+W, reg, payload, STOP — i.e.
        // the standard MAX30102 sequential register write.
        [[nodiscard]] virtual int i2cWriteReg(uint8_t devAddr, uint8_t reg,
                                              const uint8_t* data, size_t len) = 0;

        // Burst read of `len` bytes starting at `reg`. The MAX30102 FIFO
        // auto-increments the read pointer, so a multi-byte read of
        // FIFO_DATA returns successive samples — the driver relies on this.
        [[nodiscard]] virtual int i2cReadReg(uint8_t devAddr, uint8_t reg,
                                             uint8_t* out, size_t len) = 0;

        // Monotonic millisecond clock used for sample timestamps. The driver
        // never assumes wall-clock semantics; only differences are taken.
        [[nodiscard]] virtual uint32_t millis() const = 0;

        // Tight microsecond-scale spin used after MODE_CONFIG.RESET to give
        // the chip a chance to clear its internal state. Implementations
        // may busy-wait on small values.
        virtual void delayUs(uint32_t us) = 0;
    };

    // Convenience single-byte writers/readers. Inlined free functions
    // rather than methods so platforms can drop straight in without a
    // virtual-call layer when only a single byte is needed.
    [[nodiscard]] inline int writeReg(IHal& hal, uint8_t addr, uint8_t reg, uint8_t val)
    {
        return hal.i2cWriteReg(addr, reg, &val, 1);
    }

    [[nodiscard]] inline int readReg(IHal& hal, uint8_t addr, uint8_t reg, uint8_t& out)
    {
        return hal.i2cReadReg(addr, reg, &out, 1);
    }
}
