#pragma once

#include <cstddef>
#include <cstdint>

// Wire-protocol formatters / parser. Two modes are supported:
//
//   1. JSON-Lines  — one `\n`-terminated record per sample. Eyeballable
//                    in any serial terminal. Default RP2040 boot mode.
//   2. Binary      — `[STX][LEN16 LE][TYPE][PAYLOAD][CRC16 LE]`
//                    framed with CRC-16/CCITT-FALSE.
//
// Frame type codes are an append-only enum (docs/PROTOCOL.md). Never
// repurpose a number; always add new ones at the next free slot.

namespace oxinode::max3010x::proto
{
    static constexpr uint8_t  kStx       = 0xAA;

    // Append-only — see CLAUDE.md / docs/PROTOCOL.md.
    static constexpr uint8_t  kTypeSample = 0x01;
    static constexpr uint8_t  kTypeStatus = 0x02;
    static constexpr uint8_t  kTypeAck    = 0x03;
    static constexpr uint8_t  kTypeCtrl   = 0xFE;

    // Payload sizes (excludes TYPE byte itself, since LEN counts TYPE).
    static constexpr size_t   kPayloadSample = 13;  // 14 bytes including TYPE
    static constexpr size_t   kPayloadStatus = 3;
    static constexpr size_t   kPayloadAck    = 0;
    static constexpr size_t   kPayloadCtrl   = 0;

    // CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflect, no xorout.
    [[nodiscard]] uint16_t crc16(const uint8_t* data, size_t len);

    // ── Sample frame layout (TYPE 0x01) ─────────────────────────────
    //   off  size  field
    //    0    4    t_ms        (uint32_t LE)
    //    4    4    ir          (uint32_t LE — only low 18 bits significant)
    //    8    4    red         (uint32_t LE)
    //   12    1    hr          (uint8_t  BPM, 0 = invalid)
    //   13    1    spo2        (uint8_t  %,   0 = invalid)
    //   ─── 14 bytes (1 TYPE + 13 payload) ───
    static constexpr size_t kSampleFrameWireLen = 1 + 2 + 14 + 2;  // STX+LEN+(TYPE+payload)+CRC

    // ── JSON-Lines formatter ────────────────────────────────────────
    class JsonLineFormatter
    {
    public:
        // Writes `{"t":...,"ir":...,"red":...,"hr":...,"spo2":...}\n`.
        // Returns bytes written, or -1 if `bufLen` is too small. Caller
        // owns `buf`; no NUL terminator is appended (binary-safe).
        [[nodiscard]] static int formatSample(char* buf, size_t bufLen,
                                              uint32_t tMs,
                                              uint32_t ir, uint32_t red,
                                              uint8_t hrBpm, uint8_t spo2Pct);
    };

    // ── Binary framer ───────────────────────────────────────────────
    class BinFramer
    {
    public:
        // Build a SAMPLE frame into `buf`. Returns frame length in bytes
        // (== kSampleFrameWireLen on success), or -1 if `bufLen` short.
        [[nodiscard]] static int buildSample(uint8_t* buf, size_t bufLen,
                                             uint32_t tMs,
                                             uint32_t ir, uint32_t red,
                                             uint8_t hrBpm, uint8_t spo2Pct);
    };

    // ── Parser ──────────────────────────────────────────────────────

    enum class ParseStatus : int8_t
    {
        Ok           =  0,
        NeedMoreData =  1,  // not enough bytes for a full frame yet
        BadStx       = -1,
        BadCrc       = -2,
        BadLen       = -3,
    };

    struct ParsedFrame
    {
        uint8_t        type = 0;
        const uint8_t* payload = nullptr;
        size_t         payloadLen = 0;
        size_t         consumed = 0;  // bytes the caller may discard
    };

    // Parse exactly one frame out of `data` of length `len`. On
    // NeedMoreData the caller should accumulate more bytes and retry;
    // on BadStx/BadCrc the caller should advance `consumed` (1 byte
    // re-sync) and retry. The returned `payload` pointer is into
    // `data`; it stays valid for as long as `data` does.
    [[nodiscard]] ParseStatus parseFrame(const uint8_t* data, size_t len,
                                         ParsedFrame& out);
}
