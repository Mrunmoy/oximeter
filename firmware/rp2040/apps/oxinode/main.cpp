// OxiNode — RP2040-Zero firmware top-level.
//
// Boot flow
//   core0 : stdio_init_all() → spawn core1 → USB pump + 1 Hz alive
//   core1 : init HAL → init MAX30102 → wait on FIFO INT, drain FIFO
//
// The MAX30102 drives the INT line low when AFULL fires; the GPIO
// ISR posts a token into the inter-core FIFO. Core1 unblocks, calls
// driver.handleInterrupt(), which fans samples out to the HR / SpO2
// algorithms (owned by the driver) and to the USB-CDC link via a
// SampleObserver implemented here.
//
// Anything that isn't time-critical (host control parsing, alive
// frame, banner) lives on core0 so the sample-drain loop stays
// deterministic.

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "board/pins.hpp"
#include "max3010x/Max30102.hpp"
#include "max3010x/Registers.hpp"
#include "max3010x/SampleObserver.hpp"

#include "PicoI2cHal.hpp"
#include "PicoIntPin.hpp"
#include "StatusLog.hpp"
#include "UsbCdcLink.hpp"

#ifndef OXINODE_BUILD_VERSION
#define OXINODE_BUILD_VERSION "dev"
#endif

namespace
{
    using oxinode::board::kI2cFreqHz;
    using oxinode::board::kMax3010xI2cAddr;
    using oxinode::board::kPinInt;
    using oxinode::board::kPinScl;
    using oxinode::board::kPinSda;

    using oxinode::rp2040::PicoI2cHal;
    using oxinode::rp2040::PicoIntPin;
    using oxinode::rp2040::StatusLog;
    using oxinode::rp2040::UsbCdcLink;

    using oxinode::max3010x::AdcRange;
    using oxinode::max3010x::ISampleObserver;
    using oxinode::max3010x::Max30102;
    using oxinode::max3010x::Mode;
    using oxinode::max3010x::PulseWidth;
    using oxinode::max3010x::SampleAveraging;
    using oxinode::max3010x::SampleRate;

    // ── Cross-core state ──────────────────────────────────────
    std::atomic<std::int16_t> g_lastHr{-1};
    std::atomic<std::int16_t> g_lastSpo2{-1};
    std::atomic<bool>         g_core1Ready{false};

    // The link is owned by core0 but written to from core1. The
    // pico_stdio_usb / TinyUSB FIFO is its own synchronisation
    // domain, so a non-atomic pointer is sufficient here — the
    // pointer itself never changes once set up.
    UsbCdcLink* g_link = nullptr;

    // ── SampleObserver — bridges the driver to UsbCdcLink ─────
    class LinkObserver final : public ISampleObserver
    {
    public:
        void onSample(std::uint32_t tMs, std::uint32_t ir, std::uint32_t red) override
        {
            // hr / spo2 surface from onHrSpo2 below; for the per-
            // sample frame we publish the most recent estimate.
            const std::int16_t hr   = g_lastHr.load(std::memory_order_relaxed);
            const std::int16_t spo2 = g_lastSpo2.load(std::memory_order_relaxed);
            if (g_link != nullptr)
            {
                g_link->writeSample(tMs, ir, red, hr, spo2);
            }
        }

        void onHrSpo2(std::uint32_t /*tMs*/, std::uint8_t hrBpm, std::uint8_t spo2Pct) override
        {
            // 0 → "not yet valid" per the driver contract. Promote
            // to -1 so the JSON consumer can render it as `null`.
            const std::int16_t hr   = (hrBpm   == 0u) ? std::int16_t{-1} : static_cast<std::int16_t>(hrBpm);
            const std::int16_t spo2 = (spo2Pct == 0u) ? std::int16_t{-1} : static_cast<std::int16_t>(spo2Pct);
            g_lastHr.store(hr,     std::memory_order_relaxed);
            g_lastSpo2.store(spo2, std::memory_order_relaxed);
        }
    };

    LinkObserver g_observer;

    // ── Driver configuration ──────────────────────────────────
    // SpO2 mode (red + IR), 100 Hz output, 4× sample averaging
    // (matches the Maxim reference algo). FIFO almost-full
    // threshold of 17 → AFULL fires when 15 unread entries are
    // buffered, leaving headroom for I²C latency before overflow.
    Max30102::Config makeSensorConfig()
    {
        Max30102::Config cfg{};
        cfg.devAddr                  = kMax3010xI2cAddr;
        cfg.mode                     = Mode::Spo2;
        cfg.avg                      = SampleAveraging::AVG_4;
        cfg.rate                     = SampleRate::SR_100;
        cfg.pulseWidth               = PulseWidth::PW_411_18BIT;
        cfg.adcRange                 = AdcRange::RANGE_4096;
        cfg.fifoAlmostFullThreshold  = 17;
        cfg.fifoRollover             = true;
        return cfg;
    }

    void printBanner()
    {
        // Plain printf — runs in JSON mode by default; the host
        // sees this as a single self-describing JSON object.
        std::printf("{\"status\":\"boot\",\"build\":\"%s\","
                    "\"sda\":%u,\"scl\":%u,\"int\":%u,"
                    "\"i2c_hz\":%u}\n",
                    OXINODE_BUILD_VERSION,
                    kPinSda,
                    kPinScl,
                    kPinInt,
                    static_cast<unsigned>(kI2cFreqHz));
    }

    // ── core1: sample-drain loop ───────────────────────────────
    void core1_entry()
    {
        // HAL must be initialised on the same core that uses it,
        // because pico-sdk's i2c IRQ binding is per-core.
        PicoI2cHal hal(PicoI2cHal::Config{
            /*i2cIndex*/ 0,
            /*sdaPin  */ kPinSda,
            /*sclPin  */ kPinScl,
            /*freqHz  */ kI2cFreqHz,
            /*devAddr */ kMax3010xI2cAddr,
        });
        hal.init();

        Max30102 sensor(hal);
        sensor.addObserver(&g_observer);

        if (const int rc = sensor.probe(); rc != 0)
        {
            StatusLog::warn("sensor_probe_failed", "rc=%d", rc);
            // Fall through: AFULL won't ever fire if probe failed,
            // but keep the FIFO loop alive so a later cable insert
            // can still bring the device up after a power cycle.
        }

        const Max30102::Config cfg = makeSensorConfig();
        if (const int rc = sensor.configure(cfg); rc != 0)
        {
            StatusLog::warn("sensor_configure_failed", "rc=%d", rc);
        }

        PicoIntPin::init(kPinInt);
        g_core1Ready.store(true, std::memory_order_release);

        for (;;)
        {
            const std::uint32_t edges = PicoIntPin::waitForInterrupt();
            (void)edges;

            // The driver reads INTR_STATUS_{1,2}, drains the FIFO,
            // updates HR/SpO2, and fans samples to all observers
            // we registered above.
            const int n = sensor.handleInterrupt();
            if (n < 0)
            {
                StatusLog::warn("sensor_isr_drain_failed", "rc=%d", n);
            }
        }
    }
}

int main()
{
    stdio_init_all();

    // Give the host CDC enumerator a moment so the banner lands in
    // the user's terminal instead of being lost on cold boot.
    sleep_ms(500);

    UsbCdcLink link;
    link.init();

    StatusLog::bind(&link);
    g_link = &link;

    printBanner();

    multicore_launch_core1(&core1_entry);

    // Wait for core1 to stand up before we start emitting alive
    // frames; otherwise the first one races sensor.configure().
    while (!g_core1Ready.load(std::memory_order_acquire))
    {
        sleep_ms(5);
    }

    StatusLog::info("ready", "build=%s", OXINODE_BUILD_VERSION);

    absolute_time_t nextAlive = make_timeout_time_ms(1000);
    for (;;)
    {
        // Cheap and frequent: poll host CDC for control commands.
        // The repeating timer in UsbCdcLink::init() keeps tud_task()
        // serviced; this loop doesn't need to.
        link.pollHostInput();

        if (absolute_time_diff_us(get_absolute_time(), nextAlive) <= 0)
        {
            const std::uint32_t now    = to_ms_since_boot(get_absolute_time());
            const std::uint32_t edges  = PicoIntPin::edgeCount();
            const std::int16_t  hrVal  = g_lastHr.load(std::memory_order_relaxed);
            const std::int16_t  spo2V  = g_lastSpo2.load(std::memory_order_relaxed);
            link.writeAlive(now, edges, hrVal, spo2V);
            nextAlive = make_timeout_time_ms(1000);
        }

        // Yield until the next IRQ (USB or timer). Avoids burning
        // current spinning when nothing's up.
        __wfe();
    }
}
