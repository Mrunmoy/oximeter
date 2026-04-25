#include "max3010x/Framer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace
{
    namespace proto = oxinode::max3010x::proto;

    // ── CRC reference vectors ──────────────────────────────────────────

    TEST(Crc16Test, KnownVectors)
    {
        // CRC-16/CCITT-FALSE of "123456789" → 0x29B1 (Boost.CRC,
        // crccalc.com — universal reference value).
        const char* k = "123456789";
        EXPECT_EQ(0x29B1,
                  proto::crc16(reinterpret_cast<const uint8_t*>(k), 9));

        // Empty input → init value 0xFFFF.
        EXPECT_EQ(0xFFFF, proto::crc16(nullptr, 0));
    }

    // ── JSON-Lines ─────────────────────────────────────────────────────

    TEST(JsonLineTest, FormatsExpectedKeys)
    {
        char buf[128] = {};
        const int n = proto::JsonLineFormatter::formatSample(
            buf, sizeof(buf),
            12345u, 123456u, 98765u, 72u, 98u);
        ASSERT_GT(n, 0);
        const std::string s(buf, static_cast<size_t>(n));

        // Exactly one trailing newline, and not embedded mid-record.
        EXPECT_EQ('\n', s.back());
        EXPECT_EQ(1u, std::count(s.begin(), s.end(), '\n'));

        // All required keys present.
        EXPECT_NE(std::string::npos, s.find("\"t\":12345"));
        EXPECT_NE(std::string::npos, s.find("\"ir\":123456"));
        EXPECT_NE(std::string::npos, s.find("\"red\":98765"));
        EXPECT_NE(std::string::npos, s.find("\"hr\":72"));
        EXPECT_NE(std::string::npos, s.find("\"spo2\":98"));
    }

    TEST(JsonLineTest, BufferTooSmallReturnsNegative)
    {
        char buf[8] = {};
        const int n = proto::JsonLineFormatter::formatSample(
            buf, sizeof(buf), 1, 2, 3, 4, 5);
        EXPECT_LT(n, 0);
    }

    // ── Binary framer round-trip ───────────────────────────────────────

    TEST(BinFramerTest, BuildAndParseRoundTrip)
    {
        uint8_t buf[64] = {};
        const int n = proto::BinFramer::buildSample(
            buf, sizeof(buf),
            0xDEADBEEFu, 0x12345u, 0x6789Au, 75, 96);
        ASSERT_EQ(static_cast<int>(proto::kSampleFrameWireLen), n);

        proto::ParsedFrame f;
        const proto::ParseStatus s = proto::parseFrame(buf, static_cast<size_t>(n), f);
        ASSERT_EQ(proto::ParseStatus::Ok, s);
        EXPECT_EQ(proto::kTypeSample, f.type);
        EXPECT_EQ(proto::kPayloadSample, f.payloadLen);
        EXPECT_EQ(static_cast<size_t>(n), f.consumed);

        // Decode payload back and check round-trip.
        const uint8_t* p = f.payload;
        const uint32_t t =
            static_cast<uint32_t>(p[0]) |
            (static_cast<uint32_t>(p[1]) << 8) |
            (static_cast<uint32_t>(p[2]) << 16) |
            (static_cast<uint32_t>(p[3]) << 24);
        const uint32_t ir =
            static_cast<uint32_t>(p[4]) |
            (static_cast<uint32_t>(p[5]) << 8) |
            (static_cast<uint32_t>(p[6]) << 16);
        const uint32_t red =
            static_cast<uint32_t>(p[7]) |
            (static_cast<uint32_t>(p[8]) << 8) |
            (static_cast<uint32_t>(p[9]) << 16);
        EXPECT_EQ(0xDEADBEEFu, t);
        EXPECT_EQ(0x12345u, ir);
        EXPECT_EQ(0x6789Au, red);
        EXPECT_EQ(75u, p[10]);
        EXPECT_EQ(96u, p[11]);
    }

    TEST(BinFramerTest, RejectsBadCrc)
    {
        uint8_t buf[64] = {};
        const int n = proto::BinFramer::buildSample(
            buf, sizeof(buf), 1, 2, 3, 4, 5);
        ASSERT_GT(n, 0);

        // Flip a payload byte; CRC is now stale.
        buf[5] ^= 0xFF;
        proto::ParsedFrame f;
        EXPECT_EQ(proto::ParseStatus::BadCrc,
                  proto::parseFrame(buf, static_cast<size_t>(n), f));
    }

    TEST(BinFramerTest, NeedMoreBytesOnTruncated)
    {
        uint8_t buf[64] = {};
        const int n = proto::BinFramer::buildSample(
            buf, sizeof(buf), 1, 2, 3, 4, 5);
        ASSERT_GT(n, 0);

        proto::ParsedFrame f;
        // Drop the last byte → parser must say "need more".
        EXPECT_EQ(proto::ParseStatus::NeedMoreData,
                  proto::parseFrame(buf, static_cast<size_t>(n - 1), f));
        // Header-only — also "need more".
        EXPECT_EQ(proto::ParseStatus::NeedMoreData,
                  proto::parseFrame(buf, 2, f));
    }

    TEST(BinFramerTest, BadStxResyncs)
    {
        uint8_t buf[64] = {};
        const int n = proto::BinFramer::buildSample(
            buf, sizeof(buf), 1, 2, 3, 4, 5);
        ASSERT_GT(n, 0);

        // Garbage byte before STX — caller's byte stream often starts
        // mid-frame on first connect.
        uint8_t with_garbage[80] = {};
        with_garbage[0] = 0x55;  // not STX
        std::memcpy(&with_garbage[1], buf, static_cast<size_t>(n));

        proto::ParsedFrame f;
        EXPECT_EQ(proto::ParseStatus::BadStx,
                  proto::parseFrame(with_garbage,
                                    static_cast<size_t>(n + 1), f));
        EXPECT_EQ(1u, f.consumed);

        // Re-parse from offset 1 → should succeed.
        EXPECT_EQ(proto::ParseStatus::Ok,
                  proto::parseFrame(&with_garbage[f.consumed],
                                    static_cast<size_t>(n), f));
    }

    TEST(BinFramerTest, BufferTooSmallToBuildReturnsNegative)
    {
        uint8_t buf[4] = {};
        const int n = proto::BinFramer::buildSample(
            buf, sizeof(buf), 1, 2, 3, 4, 5);
        EXPECT_LT(n, 0);
    }
}
