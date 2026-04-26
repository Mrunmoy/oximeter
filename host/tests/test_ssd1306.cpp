#include "ssd1306/Font5x7.hpp"
#include "ssd1306/IBus.hpp"
#include "ssd1306/Ssd1306.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace
{
    using oxinode::ssd1306::IBus;
    using oxinode::ssd1306::Ssd1306;

    // ── Fake bus ──────────────────────────────────────────────────
    //
    // Records every write so tests can assert on the address byte,
    // the leading control byte, and the cumulative payload. `failNext`
    // burns once — the bus then succeeds again, which mirrors the
    // FakeI2cHal behaviour used by the MAX30102 tests.
    class FakeSsd1306Bus : public IBus
    {
    public:
        struct Transaction
        {
            std::uint8_t              addr = 0;
            std::vector<std::uint8_t> bytes;
        };

        int write(std::uint8_t addr, const std::uint8_t* data, std::size_t len) override
        {
            if (m_failNext)
            {
                m_failNext = false;
                return -1;
            }
            Transaction t;
            t.addr = addr;
            t.bytes.assign(data, data + len);
            m_log.push_back(std::move(t));
            return 0;
        }

        const std::vector<Transaction>& log() const { return m_log; }
        void clearLog() { m_log.clear(); }
        void failNext()  { m_failNext = true; }

    private:
        std::vector<Transaction> m_log;
        bool                     m_failNext = false;
    };

    // ── Tests ─────────────────────────────────────────────────────

    TEST(Ssd1306Test, InitSendsCommandsAndReachesDisplayOn)
    {
        FakeSsd1306Bus bus;
        Ssd1306 oled(bus, Ssd1306::kDefaultI2cAddr);

        EXPECT_EQ(0, oled.init());

        ASSERT_FALSE(bus.log().empty());

        bool sawDisplayOn = false;
        for (const FakeSsd1306Bus::Transaction& t : bus.log())
        {
            EXPECT_EQ(Ssd1306::kDefaultI2cAddr, t.addr);
            ASSERT_FALSE(t.bytes.empty());
            // Init only ever talks command stream; first byte must be
            // the command-stream control byte (Co=0, D/C#=0).
            EXPECT_EQ(0x00, t.bytes.front());

            for (std::size_t i = 1; i < t.bytes.size(); ++i)
            {
                if (t.bytes[i] == 0xAF)   // SSD1306_DISPLAYON
                {
                    sawDisplayOn = true;
                }
            }
        }
        EXPECT_TRUE(sawDisplayOn) << "init() never issued DISPLAY_ON (0xAF)";
    }

    TEST(Ssd1306Test, InitPropagatesBusFailure)
    {
        FakeSsd1306Bus bus;
        Ssd1306 oled(bus);

        bus.failNext();
        EXPECT_NE(0, oled.init());
    }

    TEST(Ssd1306Test, ClearZeroesFramebuffer)
    {
        FakeSsd1306Bus bus;
        Ssd1306 oled(bus);

        oled.drawPixel(0, 0, true);
        oled.drawPixel(10, 12, true);
        oled.drawPixel(127, 63, true);
        oled.clear();

        const std::uint8_t* fb = oled.framebuffer();
        for (std::size_t i = 0; i < Ssd1306::kFramebufBytes; ++i)
        {
            EXPECT_EQ(0u, fb[i]) << "fb[" << i << "] not cleared";
        }
    }

    TEST(Ssd1306Test, DrawPixelSetsCorrectBit)
    {
        FakeSsd1306Bus bus;
        Ssd1306 oled(bus);

        // (0,0) → page 0, byte 0, bit 0 → 0x01
        oled.drawPixel(0, 0, true);
        EXPECT_EQ(0x01, oled.framebuffer()[0]);

        // (5,9) → page 1, byte (1*kWidth + 5), bit 1 → 0x02
        oled.drawPixel(5, 9, true);
        EXPECT_EQ(0x02, oled.framebuffer()[Ssd1306::kWidth + 5]);

        // (127,63) → page 7, byte (7*kWidth + 127), bit 7 → 0x80
        oled.drawPixel(127, 63, true);
        EXPECT_EQ(0x80, oled.framebuffer()[7 * Ssd1306::kWidth + 127]);
    }

    TEST(Ssd1306Test, DrawPixelClampsOutOfBounds)
    {
        FakeSsd1306Bus bus;
        Ssd1306 oled(bus);

        oled.drawPixel(-1, 0, true);
        oled.drawPixel(0, -1, true);
        oled.drawPixel(-5, -7, true);
        oled.drawPixel(Ssd1306::kWidth, 0, true);
        oled.drawPixel(0, Ssd1306::kHeight, true);
        oled.drawPixel(Ssd1306::kWidth + 10, Ssd1306::kHeight + 10, true);

        const std::uint8_t* fb = oled.framebuffer();
        for (std::size_t i = 0; i < Ssd1306::kFramebufBytes; ++i)
        {
            EXPECT_EQ(0u, fb[i]) << "out-of-bounds drawPixel touched fb[" << i << "]";
        }
    }

    TEST(Ssd1306Test, DrawTextRendersGlyphA)
    {
        FakeSsd1306Bus bus;
        Ssd1306 oled(bus);

        oled.drawText(0, 0, "A");

        const std::uint8_t* fb = oled.framebuffer();
        EXPECT_EQ(0x7E, fb[0]);
        EXPECT_EQ(0x11, fb[1]);
        EXPECT_EQ(0x11, fb[2]);
        EXPECT_EQ(0x11, fb[3]);
        EXPECT_EQ(0x7E, fb[4]);
        // Inter-character spacing column must be left blank.
        EXPECT_EQ(0x00, fb[5]);
    }

    TEST(Ssd1306Test, DrawTextHandlesNullPointer)
    {
        FakeSsd1306Bus bus;
        Ssd1306 oled(bus);

        oled.drawText(0, 0, nullptr);

        const std::uint8_t* fb = oled.framebuffer();
        for (std::size_t i = 0; i < Ssd1306::kFramebufBytes; ++i)
        {
            EXPECT_EQ(0u, fb[i]) << "nullptr text touched fb[" << i << "]";
        }
    }

    TEST(Ssd1306Test, FlushSendsFullFramebufferBytes)
    {
        FakeSsd1306Bus bus;
        Ssd1306 oled(bus);

        EXPECT_EQ(0, oled.flush());

        std::size_t dataBytes = 0;
        for (const FakeSsd1306Bus::Transaction& t : bus.log())
        {
            ASSERT_FALSE(t.bytes.empty());
            if (t.bytes.front() == 0x40)   // data-stream control byte
            {
                dataBytes += t.bytes.size() - 1;
            }
        }
        EXPECT_EQ(Ssd1306::kFramebufBytes, dataBytes);
    }

    TEST(Ssd1306Test, FlushSetsAddressingWindow)
    {
        FakeSsd1306Bus bus;
        Ssd1306 oled(bus);

        EXPECT_EQ(0, oled.flush());

        bool sawSetColAddr  = false;   // 0x21
        bool sawSetPageAddr = false;   // 0x22
        for (const FakeSsd1306Bus::Transaction& t : bus.log())
        {
            ASSERT_FALSE(t.bytes.empty());
            if (t.bytes.front() != 0x00)
            {
                continue;
            }
            for (std::size_t i = 1; i < t.bytes.size(); ++i)
            {
                if (t.bytes[i] == 0x21) { sawSetColAddr  = true; }
                if (t.bytes[i] == 0x22) { sawSetPageAddr = true; }
            }
        }
        EXPECT_TRUE(sawSetColAddr)  << "flush() never issued SET_COLUMN_ADDR (0x21)";
        EXPECT_TRUE(sawSetPageAddr) << "flush() never issued SET_PAGE_ADDR (0x22)";
    }

    TEST(Ssd1306Test, FlushPropagatesBusFailure)
    {
        FakeSsd1306Bus bus;
        Ssd1306 oled(bus);

        bus.failNext();
        EXPECT_NE(0, oled.flush());
    }

    TEST(Ssd1306Test, DashboardLeavesNonZeroFramebuffer)
    {
        FakeSsd1306Bus bus;
        Ssd1306 oled(bus);

        oled.drawDashboard(75, 98, "v1.0");

        const std::uint8_t* fb = oled.framebuffer();
        bool anyNonZero = false;
        for (std::size_t i = 0; i < Ssd1306::kFramebufBytes; ++i)
        {
            if (fb[i] != 0u)
            {
                anyNonZero = true;
                break;
            }
        }
        EXPECT_TRUE(anyNonZero) << "drawDashboard left an empty framebuffer";
    }
}
