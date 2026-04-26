#pragma once

#include <cstdint>

// MAX30102 register map and register-field encodings. Sourced from the
// Maxim/Analog Devices MAX30102 datasheet (Rev 1, 10/2018), publicly
// republished by ADI. Fields the driver actually drives are listed; pad
// registers (0x0B, 0x10, etc.) are intentionally omitted because they
// either read 0 or aren't documented.

namespace oxinode::max3010x::reg
{
    // ── Default I²C address (7-bit) ─────────────────────────────────────
    static constexpr uint8_t kI2cAddr = 0x57;
    static constexpr uint8_t kPartId  = 0x15;

    // ── Register addresses ──────────────────────────────────────────────
    static constexpr uint8_t INTR_STATUS_1   = 0x00;
    static constexpr uint8_t INTR_STATUS_2   = 0x01;
    static constexpr uint8_t INTR_ENABLE_1   = 0x02;
    static constexpr uint8_t INTR_ENABLE_2   = 0x03;
    static constexpr uint8_t FIFO_WR_PTR     = 0x04;
    static constexpr uint8_t OVF_COUNTER     = 0x05;
    static constexpr uint8_t FIFO_RD_PTR     = 0x06;
    static constexpr uint8_t FIFO_DATA       = 0x07;
    static constexpr uint8_t FIFO_CONFIG     = 0x08;
    static constexpr uint8_t MODE_CONFIG     = 0x09;
    static constexpr uint8_t SPO2_CONFIG     = 0x0A;
    static constexpr uint8_t LED1_PA         = 0x0C;  // RED
    static constexpr uint8_t LED2_PA         = 0x0D;  // IR
    static constexpr uint8_t MULTI_LED_CTRL1 = 0x11;
    static constexpr uint8_t MULTI_LED_CTRL2 = 0x12;
    static constexpr uint8_t TEMP_INT        = 0x1F;
    static constexpr uint8_t TEMP_FRAC       = 0x20;
    static constexpr uint8_t TEMP_CONFIG     = 0x21;
    static constexpr uint8_t REV_ID          = 0xFE;
    static constexpr uint8_t PART_ID         = 0xFF;

    // ── INTR_STATUS / INTR_ENABLE bits ──────────────────────────────────
    static constexpr uint8_t INT_A_FULL      = 1 << 7;  // FIFO almost full
    static constexpr uint8_t INT_PPG_RDY     = 1 << 6;  // new sample ready
    static constexpr uint8_t INT_ALC_OVF     = 1 << 5;  // ambient-light overflow
    static constexpr uint8_t INT_PWR_RDY     = 1 << 0;  // power-ready (status 1)
    static constexpr uint8_t INT_DIE_TEMP    = 1 << 1;  // die-temp ready (reg 2)

    // ── MODE_CONFIG values ──────────────────────────────────────────────
    static constexpr uint8_t MODE_RESET      = 1 << 6;
    static constexpr uint8_t MODE_SHUTDOWN   = 1 << 7;
    static constexpr uint8_t MODE_HR_ONLY    = 0x02;
    static constexpr uint8_t MODE_SPO2       = 0x03;
    static constexpr uint8_t MODE_MULTI_LED  = 0x07;

    // ── TEMP_CONFIG ─────────────────────────────────────────────────────
    static constexpr uint8_t TEMP_EN         = 0x01;

    // ── FIFO geometry ───────────────────────────────────────────────────
    // 32 entries × 6 bytes (RED 3-byte + IR 3-byte) in SpO2 mode.
    static constexpr uint8_t  kFifoDepth        = 32;
    static constexpr uint8_t  kBytesPerEntrySpo2 = 6;
    static constexpr uint8_t  kBytesPerEntryHr   = 3;
}

namespace oxinode::max3010x
{
    // ── Configurable enums (datasheet Table 11/12) ──────────────────────

    // Sample averaging: FIFO_CONFIG[7:5] — chip averages N raw samples
    // before pushing one entry, trading bandwidth for noise.
    enum class SampleAveraging : uint8_t
    {
        AVG_1  = 0b000,
        AVG_2  = 0b001,
        AVG_4  = 0b010,
        AVG_8  = 0b011,
        AVG_16 = 0b100,
        AVG_32 = 0b101,
    };

    // SPO2_CONFIG[4:2] — output sample rate.
    enum class SampleRate : uint8_t
    {
        SR_50   = 0b000,
        SR_100  = 0b001,  // default
        SR_200  = 0b010,
        SR_400  = 0b011,
        SR_800  = 0b100,
        SR_1000 = 0b101,
        SR_1600 = 0b110,
        SR_3200 = 0b111,
    };

    // SPO2_CONFIG[1:0] — LED pulse width / ADC resolution.
    enum class PulseWidth : uint8_t
    {
        PW_69_15BIT  = 0b00,
        PW_118_16BIT = 0b01,
        PW_215_17BIT = 0b10,
        PW_411_18BIT = 0b11,  // default for full-resolution SpO2
    };

    // SPO2_CONFIG[6:5] — ADC full-scale range (LSB nA).
    enum class AdcRange : uint8_t
    {
        RANGE_2048  = 0b00,
        RANGE_4096  = 0b01,
        RANGE_8192  = 0b10,
        RANGE_16384 = 0b11,
    };

    // MODE_CONFIG[2:0] — operating mode.
    enum class Mode : uint8_t
    {
        HrOnly   = reg::MODE_HR_ONLY,
        Spo2     = reg::MODE_SPO2,
        MultiLed = reg::MODE_MULTI_LED,
    };

    // FIFO_CONFIG[3:0] — FIFO_A_FULL trigger threshold.
    //
    // Encoding (datasheet §FIFO Almost Full Threshold, p.14): the
    // chip fires the FIFO_A_FULL interrupt when (32 - N) entries are
    // unread, where N is the raw register value. N is 4 bits wide
    // (0..15), so the reachable trigger range is **17..32 unread
    // entries**. Asking for a smaller threshold (e.g. "fire at 15
    // unread") is not expressible on the wire — the field has no
    // encoding for it.
    //
    // The wire field's 4-bit width is the entire reason for this
    // type. Storing the threshold as a bare `uint8_t` (the original
    // `Config::fifoAlmostFullThreshold`) was a footgun: setting it
    // to a raw value of 17 (0x11) silently masks down to 1, which
    // programs the chip to trigger at 31 unread instead of the
    // 17-unread default that the user actually wanted (matching the
    // chip's POR behaviour). Strong typing makes that class of bug a
    // compile error: you can't write `Config{}.fifoAFull = 17;`.
    //
    // Helper: `fifoAFullOnUnread<N>()` builds a value that triggers
    // at exactly N unread, with a compile-time check that N is in
    // [17, 32]. For one-off uses outside that range (there aren't
    // any; the chip just doesn't support it), cast a u8 explicitly
    // and own the consequences.
    enum class FifoAFull : uint8_t
    {
        OnFull             = 0x00,  // 32 unread (FIFO completely full)
        Unread31           = 0x01,
        Unread30           = 0x02,
        Unread29           = 0x03,
        Unread28           = 0x04,
        Unread27           = 0x05,
        Unread26           = 0x06,
        Unread25           = 0x07,
        Unread24           = 0x08,
        Unread23           = 0x09,
        Unread22           = 0x0A,
        Unread21           = 0x0B,
        Unread20           = 0x0C,
        Unread19           = 0x0D,
        Unread18           = 0x0E,
        Unread17           = 0x0F,  // chip default ("almost full")
    };
    static_assert(static_cast<uint8_t>(FifoAFull::Unread17) == 0x0F,
                  "FIFO_A_FULL is a 4-bit field");
    static_assert(static_cast<uint8_t>(FifoAFull::OnFull) == 0x00,
                  "FIFO_A_FULL=0 means 32 unread (fully full)");

    // Compile-time-checked convenience: build a `FifoAFull` from a
    // desired unread-count threshold. Out-of-range counts (< 17 or
    // > 32) are rejected at the point of use, not silently masked.
    template <int kUnread>
    [[nodiscard]] constexpr FifoAFull fifoAFullOnUnread()
    {
        static_assert(kUnread >= 17 && kUnread <= 32,
                      "FIFO_A_FULL only reachable for unread in [17, 32]");
        return static_cast<FifoAFull>(static_cast<uint8_t>(32 - kUnread));
    }
}
