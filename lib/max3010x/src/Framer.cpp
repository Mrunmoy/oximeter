#include "max3010x/Framer.hpp"

#include <cstdio>
#include <cstring>

namespace oxinode::max3010x::proto
{
    // CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflect, no xorout.
    // Bitwise implementation — host-tested code, table lookup is not
    // worth the .rodata footprint when binary frames are ~20 B each.
    uint16_t crc16(const uint8_t* data, size_t len)
    {
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < len; ++i)
        {
            crc ^= static_cast<uint16_t>(data[i]) << 8;
            for (int b = 0; b < 8; ++b)
            {
                if (crc & 0x8000u)
                {
                    crc = static_cast<uint16_t>((crc << 1) ^ 0x1021u);
                }
                else
                {
                    crc = static_cast<uint16_t>(crc << 1);
                }
            }
        }
        return crc;
    }

    // ── JSON-Lines ─────────────────────────────────────────────────────

    int JsonLineFormatter::formatSample(char* buf, size_t bufLen,
                                        uint32_t tMs,
                                        uint32_t ir, uint32_t red,
                                        uint8_t hrBpm, uint8_t spo2Pct)
    {
        // snprintf is non-throwing in practice and is the only way to
        // format integers without dynamic allocation. We accept the
        // C-stdio dependency — every libc has it for free.
        const int n = std::snprintf(
            buf, bufLen,
            "{\"t\":%lu,\"ir\":%lu,\"red\":%lu,\"hr\":%u,\"spo2\":%u}\n",
            static_cast<unsigned long>(tMs),
            static_cast<unsigned long>(ir),
            static_cast<unsigned long>(red),
            static_cast<unsigned>(hrBpm),
            static_cast<unsigned>(spo2Pct));
        if (n < 0)                        { return -1; }
        // snprintf returns "would-be" length; if it equals bufLen the
        // last byte was sacrificed for the implicit NUL — we treat
        // that as truncation since callers expect the full record.
        if (static_cast<size_t>(n) >= bufLen) { return -1; }
        return n;
    }

    // ── Binary framer ──────────────────────────────────────────────────

    static void putLe16(uint8_t* p, uint16_t v)
    {
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    }

    static void putLe32(uint8_t* p, uint32_t v)
    {
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    }

    static uint16_t getLe16(const uint8_t* p)
    {
        return static_cast<uint16_t>(
            static_cast<uint16_t>(p[0]) |
            (static_cast<uint16_t>(p[1]) << 8));
    }

    int BinFramer::buildSample(uint8_t* buf, size_t bufLen,
                               uint32_t tMs,
                               uint32_t ir, uint32_t red,
                               uint8_t hrBpm, uint8_t spo2Pct)
    {
        if (bufLen < kSampleFrameWireLen) { return -1; }

        // Wire layout:
        //   STX | LEN16 LE | TYPE | PAYLOAD(13) | CRC16 LE
        //
        // LEN counts TYPE+PAYLOAD per the protocol spec (NOT STX,
        // NOT LEN itself, NOT CRC). CRC is computed over LEN+TYPE+
        // PAYLOAD — STX is excluded so a corrupted STX never fakes
        // a valid CRC.
        //
        // Payload (13 B):
        //   off  size  field
        //    0    4    t_ms (LE)
        //    4    3    ir   (LE, low 24 bits — only 18 are significant)
        //    7    3    red  (LE)
        //   10    1    hr   BPM (0 = invalid)
        //   11    1    spo2 %  (0 = invalid)
        //   12    1    reserved (0)
        //
        // The 3-byte channel encoding mirrors the MAX30102's native
        // FIFO sample size — keeps the wire compact and matches the
        // sensor's natural resolution exactly.
        buf[0] = kStx;
        const uint16_t fieldLen = static_cast<uint16_t>(1 + kPayloadSample);
        putLe16(&buf[1], fieldLen);
        buf[3] = kTypeSample;

        uint8_t* payload = &buf[4];
        std::memset(payload, 0, kPayloadSample);
        putLe32(&payload[0], tMs);
        payload[4] = static_cast<uint8_t>( ir         & 0xFF);
        payload[5] = static_cast<uint8_t>((ir  >> 8)  & 0xFF);
        payload[6] = static_cast<uint8_t>((ir  >> 16) & 0xFF);
        payload[7] = static_cast<uint8_t>( red        & 0xFF);
        payload[8] = static_cast<uint8_t>((red >> 8)  & 0xFF);
        payload[9] = static_cast<uint8_t>((red >> 16) & 0xFF);
        payload[10] = hrBpm;
        payload[11] = spo2Pct;
        payload[12] = 0;

        // CRC over LEN(2) + TYPE(1) + PAYLOAD(13) — everything after
        // STX, before the CRC slot.
        const uint16_t crc = crc16(&buf[1], 2 + 1 + kPayloadSample);
        putLe16(&buf[1 + 2 + 1 + kPayloadSample], crc);

        return static_cast<int>(kSampleFrameWireLen);
    }

    // ── Parser ─────────────────────────────────────────────────────────

    ParseStatus parseFrame(const uint8_t* data, size_t len, ParsedFrame& out)
    {
        out = ParsedFrame{};

        if (len < 1) { return ParseStatus::NeedMoreData; }
        if (data[0] != kStx)
        {
            // Caller resyncs by skipping one byte. `consumed` is
            // populated even on error so the resync loop is simple.
            out.consumed = 1;
            return ParseStatus::BadStx;
        }
        if (len < 1 + 2) { return ParseStatus::NeedMoreData; }

        const uint16_t fieldLen = getLe16(&data[1]);
        // Cap at a sane upper bound so a bit-flipped LEN doesn't make
        // us wait forever for bytes that will never come.
        if (fieldLen == 0 || fieldLen > 256)
        {
            out.consumed = 1;
            return ParseStatus::BadLen;
        }

        const size_t totalLen = 1 + 2 + fieldLen + 2;  // +STX +LEN +CRC
        if (len < totalLen) { return ParseStatus::NeedMoreData; }

        const uint16_t expectCrc = getLe16(&data[1 + 2 + fieldLen]);
        const uint16_t gotCrc    = crc16(&data[1], 2u + fieldLen);
        if (expectCrc != gotCrc)
        {
            out.consumed = 1;
            return ParseStatus::BadCrc;
        }

        out.type        = data[1 + 2];
        out.payload     = &data[1 + 2 + 1];
        out.payloadLen  = static_cast<size_t>(fieldLen - 1);
        out.consumed    = totalLen;
        return ParseStatus::Ok;
    }
}
