// OxiNode — RP2040-Zero firmware top-level.
//
// Boot flow
//   core0 : stdio_init_all() → spawn core1 → USB pump + ring consumer
//   core1 : init HAL → init MAX30102 → wait on FIFO INT, drain FIFO
//           into a lock-free SPSC ring (SampleRing).
//
// Threading model (post-Stage B — DESIGN.md D-15):
//   core1 is the *producer*. It owns the I²C bus, runs the sensor
//   driver, runs the DSP (HrDetector, Spo2Algo are members of the
//   Max30102 instance), and pushes one decoded sample per drained
//   FIFO entry into `g_ring`. It never touches USB.
//   core0 is the *consumer*. Each main-loop iteration drains pending
//   samples from `g_ring`, formats one JSON line per sample, and
//   enqueues it into TinyUSB's CDC TX FIFO. Per-iteration consumer
//   latency is measured with `time_us_64()` and surfaced in the
//   alive frame; if it exceeds `kDspBudgetUs` a fault flag fires.
//
// The split means a slow USB host can no longer back-pressure the
// sensor drain — at worst the SampleRing fills, the producer
// increments `g_ringDrops`, and the alive frame surfaces it within
// 1 second. The drain loop itself stays bounded.

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/platform/sections.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "board/pins.hpp"
#include "max3010x/Max30102.hpp"
#include "max3010x/Registers.hpp"
#include "max3010x/SampleObserver.hpp"

#include "ssd1306/Ssd1306.hpp"

#include "FaultFlags.hpp"
#include "PicoI2cHal.hpp"
#include "PicoIntPin.hpp"
#include "PicoSsd1306Bus.hpp"
#include "SampleRing.hpp"
#include "StatusLog.hpp"
#include "UsbCdcLink.hpp"

#ifndef OXINODE_BUILD_VERSION
#define OXINODE_BUILD_VERSION "dev"
#endif

namespace
{
    using oxinode::board::kI2cFreqHz;
    using oxinode::board::kI2cOledFreqHz;
    using oxinode::board::kMax3010xI2cAddr;
    using oxinode::board::kOledI2cAddr;
    using oxinode::board::kPinInt;
    using oxinode::board::kPinOledScl;
    using oxinode::board::kPinOledSda;
    using oxinode::board::kPinScl;
    using oxinode::board::kPinSda;

    using oxinode::rp2040::PicoI2cHal;
    using oxinode::rp2040::PicoIntPin;
    using oxinode::rp2040::PicoSsd1306Bus;
    using oxinode::rp2040::SampleRing;
    using oxinode::rp2040::StatusLog;
    using oxinode::rp2040::UsbCdcLink;

    using oxinode::ssd1306::Ssd1306;

    using oxinode::rp2040::FAULT_NONE;
    using oxinode::rp2040::FAULT_BROWNOUT;
    using oxinode::rp2040::FAULT_ALC_DEGRADED;
    using oxinode::rp2040::FAULT_I2C_ERR;
    using oxinode::rp2040::FAULT_RING_DROPS;
    using oxinode::rp2040::FAULT_BACKPRESSURE;
    using oxinode::rp2040::FAULT_DSP_OVERBUDGET;
    using oxinode::rp2040::FAULT_STAGNANT_PRODUCER;
    using oxinode::rp2040::FAULT_STAGNANT_CONSUMER;

    using oxinode::max3010x::AdcRange;
    using oxinode::max3010x::FifoAFull;
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

    // Diagnostics surfaced in every alive frame. 127 = "not yet attempted".
    std::atomic<std::int8_t>  g_probeRc{127};
    std::atomic<std::int8_t>  g_configureRc{127};
    std::atomic<std::int32_t> g_lastDrainRc{0};
    // INTR_STATUS_{1,2} captured by core1 after each drain so core0
    // can include them in alive frames without touching the I²C bus.
    std::atomic<std::uint8_t> g_int1{0xFF};
    std::atomic<std::uint8_t> g_int2{0xFF};

    // ── Stage B: SPSC ring + producer/consumer counters ──────
    // 64 slots × 12 B/slot = 768 B of sample-payload buffer; the
    // SampleRing object also carries two `std::atomic<uint32_t>`
    // indices, so total `g_ring` size is ~776 B (compiler may pad).
    // Sized for 2× the chip's 32-deep FIFO worst-case burst plus
    // one period of consumer jitter. Power of two for mask indexing
    // in SampleRing.
    //
    // Placed in `.scratch_x.oxinode` — the dedicated 4 KB RP2040
    // SRAM bank 4 ("scratch X") at 0x20040000. Both cores can read
    // any SRAM bank, but the striped main banks (SRAM 0..3) cost an
    // extra arbitration cycle when both cores hit them at once. The
    // ring is on a textbook producer/consumer hot path: core1 writes
    // every sample (25 Hz), core0 reads every sample (25 Hz). Moving
    // it off the striped banks gives both sides a contention-free
    // path to its memory. ~776 B comfortably fits in the 4 KB bank
    // with headroom for any future per-core scratch state.
    //
    // Placement is enforced at link time by the `__scratch_x` section
    // attribute below. To verify after a build, inspect the
    // `build/rp2040/apps/oxinode/oxinode.elf.map` file: the `g_ring`
    // symbol address must fall in `[0x20040000, 0x20041000)`. There
    // is no automated host-side check (the ring's address is a
    // platform property, not visible from `host/tests/`); the
    // `linker-section-check` step below greps the .map at firmware
    // build time as a CI guard.
    __scratch_x("oxinode") SampleRing<64> g_ring;

    // Saturating max — `compare_exchange_weak` retry loop in the
    // updater since std::atomic<u32> has no `fetch_max` in C++17.
    std::atomic<std::uint32_t> g_ringDrops{0};
    std::atomic<std::uint32_t> g_ringHwm{0};
    std::atomic<std::uint32_t> g_samplesConsumed{0};
    std::atomic<std::uint32_t> g_dspUsMax{0};
    std::atomic<std::uint32_t> g_dspOverbudgetTotal{0};

    // ── Stage C: driver stats published from core1 → core0 ───
    // Max30102::stats() is a cheap value snapshot, but the Max30102
    // instance lives on core1 and the alive frame runs on core0.
    // Core1 publishes the snapshot fields into these atomics after
    // each drain so core0 can read them without touching the I²C bus.
    std::atomic<std::uint32_t> g_drvSamplesDrained{0};
    std::atomic<std::uint32_t> g_drvChipOvf{0};
    std::atomic<std::uint32_t> g_drvPwrRdy{0};
    std::atomic<std::uint32_t> g_drvAlcOvf{0};
    std::atomic<std::uint32_t> g_drvI2cErr{0};

    // CRC-16/CCITT-FALSE over the seven static config registers,
    // refreshed by core1 every kCfgCrcPeriodMs. The companion
    // `g_drvCfgCrcReadbacks` counter is the validity signal: 0 means
    // readback hasn't run yet, any non-zero means cfg_crc reflects a
    // real readback (CRC-16 itself can be 0x0000 for some inputs, so
    // the value alone is ambiguous). See DESIGN.md follow-up to D-15.
    std::atomic<std::uint16_t> g_drvCfgCrc{0};
    std::atomic<std::uint32_t> g_drvCfgCrcReadbacks{0};
    constexpr std::uint32_t    kCfgCrcPeriodMs = 10000;

    // Per-iteration consumer-side budget. Sample period at 25 Hz =
    // 40 ms; we set the alarm at 70 % of that. If a future change
    // causes the consumer body (DSP read-out + JSON encode + USB
    // enqueue) to exceed this, `g_dspOverbudgetTotal` ticks and the
    // burn-in CI gate (Stage D) trips.
    constexpr std::uint32_t kDspBudgetUs = 28000;

    template <typename T>
    void atomicMax(std::atomic<T>& a, T value) noexcept
    {
        T cur = a.load(std::memory_order_relaxed);
        while (value > cur &&
               !a.compare_exchange_weak(cur, value,
                                        std::memory_order_relaxed))
        { /* retry */ }
    }

    // The link is owned by core0. Core1 no longer writes to it —
    // the consumer loop on core0 emits all sample lines.
    UsbCdcLink* g_link = nullptr;

    // ── SampleObserver — producer side, runs on core1 ────────
    // The driver fans every decoded sample through `onSample`;
    // we push it into the SPSC ring and step on. `onHrSpo2`
    // updates the cross-core HR/SpO2 atomics that the consumer
    // reads when formatting each JSON line.
    class RingPushObserver final : public ISampleObserver
    {
    public:
        void onSample(std::uint32_t tMs, std::uint32_t ir, std::uint32_t red) override
        {
            const SampleRing<64>::Sample s{tMs, ir, red};
            if (!g_ring.tryPush(s))
            {
                g_ringDrops.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            atomicMax(g_ringHwm, g_ring.depth());
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

    RingPushObserver g_observer;

    // ── Consumer side, runs on core0 ─────────────────────────
    // Drain the ring as far as it'll go in one main-loop tick.
    // For each sample: read the latest cross-core HR/SpO2 cache,
    // format and enqueue one JSON line, and time the round-trip.
    void drainRingToUsb(UsbCdcLink& link)
    {
        SampleRing<64>::Sample s;
        while (g_ring.tryPop(s))
        {
            const std::uint64_t t0 = time_us_64();

            const std::int16_t hr   = g_lastHr.load(std::memory_order_relaxed);
            const std::int16_t spo2 = g_lastSpo2.load(std::memory_order_relaxed);
            link.writeSample(s.tMs, s.ir, s.red, hr, spo2);

            const std::uint32_t dt =
                static_cast<std::uint32_t>(time_us_64() - t0);
            atomicMax(g_dspUsMax, dt);
            if (dt > kDspBudgetUs)
            {
                g_dspOverbudgetTotal.fetch_add(1, std::memory_order_relaxed);
            }
            g_samplesConsumed.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // ── Driver configuration ──────────────────────────────────
    // Maxim's recommended profile for finger SpO2/HR — UG6409 §"Recommended
    // Practices" (p.26-27) and AN6845 step 8 (p.9). SR=100 Hz and
    // SMP_AVE=4 → 25 Hz output sample rate, which is what the Maxim
    // reference HR algorithm (UG6409 p.29-30) and SparkFun's PBA
    // example sketch are tuned for. PW=411 µs gives full 18-bit ADC
    // resolution. Driver defaults (Max30102::Config) already encode
    // the LED PA + sample-averaging values — we just hand them through.
    Max30102::Config makeSensorConfig()
    {
        Max30102::Config cfg{};
        cfg.devAddr                  = kMax3010xI2cAddr;
        cfg.mode                     = Mode::Spo2;
        cfg.avg                      = SampleAveraging::AVG_4;
        cfg.rate                     = SampleRate::SR_100;
        cfg.pulseWidth               = PulseWidth::PW_411_18BIT;
        cfg.adcRange                 = AdcRange::RANGE_4096;
        // Chip default: trigger A_FULL when 17 entries are unread.
        // Note PPG_RDY (per-sample IRQ) is also enabled by the
        // driver, so A_FULL is rarely the wake source in practice —
        // setting this conservatively keeps the latched-once-FIFO-
        // is-full safety net intact without changing steady-state
        // IRQ rate.
        cfg.fifoAFull                = FifoAFull::Unread17;
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
        static PicoI2cHal s_hal(PicoI2cHal::Config{
            /*i2cIndex*/ 0,
            /*sdaPin  */ kPinSda,
            /*sclPin  */ kPinScl,
            /*freqHz  */ kI2cFreqHz,
            /*devAddr */ kMax3010xI2cAddr,
        });
        s_hal.init();

        // PicoIntPin::init() is intentionally NOT called from this
        // core — it was already invoked from core0 in main() so the
        // GPIO IRQ trampoline runs there and the SIO wake token
        // routes correctly to this core's RX FIFO. See comment in
        // main() at the call site.
        static Max30102 s_sensor(s_hal);
        s_sensor.addObserver(&g_observer);

        const int probeRc = s_sensor.probe();
        g_probeRc.store(static_cast<std::int8_t>(probeRc), std::memory_order_relaxed);
        if (probeRc != 0)
        {
            StatusLog::warn("sensor_probe_failed", "rc=%d", probeRc);
        }

        const Max30102::Config cfg = makeSensorConfig();
        const int cfgRc = s_sensor.configure(cfg);
        g_configureRc.store(static_cast<std::int8_t>(cfgRc), std::memory_order_relaxed);
        if (cfgRc != 0)
        {
            StatusLog::warn("sensor_configure_failed", "rc=%d", cfgRc);
        }

        // PicoIntPin::init was called from core0 in main() so the
        // GPIO IRQ trampoline runs on core0. That matters: the
        // trampoline pushes a wake token via `sio_hw->fifo_wr`, which
        // *targets the OTHER core* — core0's TX FIFO is core1's RX
        // FIFO, where `multicore_fifo_pop_blocking()` below actually
        // reads from. If the GPIO IRQ were registered on core1, the
        // wake would loopback to core0 and core1 would deadlock on
        // the pop. (That was the wedge observed on the bench
        // 2026-04-26 with the IRQ-only path: 1 edge then silence.)
        //
        // Prime once after configure(): if a sample latched between
        // the LEDs arming and the IRQ becoming live, INT is already
        // low and the falling-edge IRQ won't catch it on its own.
        // One unconditional drain here reads INTR_STATUS, clears the
        // flag, and lets INT spring back high so the next sample's
        // edge fires normally.
        const int primeRc = s_sensor.handleInterrupt();
        g_lastDrainRc.store(primeRc, std::memory_order_relaxed);

        g_core1Ready.store(true, std::memory_order_release);

        // Pure IRQ-driven drain. INT fires on every new sample
        // (PPG_RDY, ~25 Hz at SR=100 × AVG=4) plus when FIFO_A_FULL
        // trips. With the trampoline correctly registered on core0,
        // the SIO wake token routes to core1 and waitForInterrupt()
        // unblocks every edge. At 25 Hz the M0+ has ~5 M cycles
        // between samples — ample headroom for the trivial ISR
        // (atomic increment + FIFO write) plus the per-sample drain
        // body (~2 ms of I²C at 100 kHz). No safety poll needed.
        absolute_time_t nextCfgCrc =
            make_timeout_time_ms(kCfgCrcPeriodMs);
        for (;;)
        {
            (void)PicoIntPin::waitForInterrupt();

            const int n = s_sensor.handleInterrupt();
            g_lastDrainRc.store(n, std::memory_order_relaxed);
            if (n < 0)
            {
                StatusLog::warn("sensor_isr_drain_failed", "rc=%d", n);
            }

            // Stage C: publish driver-internal counters to core0.
            // Max30102::stats() is a cheap-by-value snapshot; we
            // copy each field into a dedicated atomic so the alive
            // frame on core0 sees a consistent (but possibly slightly
            // stale) view without crossing the I²C boundary.
            const oxinode::max3010x::Max30102::Stats st = s_sensor.stats();
            g_drvSamplesDrained.store(st.samplesDrained, std::memory_order_relaxed);
            g_drvChipOvf       .store(st.chipOvfTotal,   std::memory_order_relaxed);
            g_drvPwrRdy        .store(st.pwrRdyEvents,   std::memory_order_relaxed);
            g_drvAlcOvf        .store(st.alcOvfEvents,   std::memory_order_relaxed);
            g_drvI2cErr        .store(st.i2cErrTotal,    std::memory_order_relaxed);

            // Snapshot INTR_STATUS_{1,2} for the alive frame on core0.
            // Cheap (one I²C transaction, ~100 µs at 100 kHz) and
            // happens on the same core that owns the I²C bus, so no
            // cross-core contention. Note: handleInterrupt() above
            // already cleared the latched flags by reading the same
            // registers, so this snapshot will normally read 0x00 —
            // only racing edges show up.
            std::uint8_t status[2] = {0xFF, 0xFF};
            (void)s_hal.i2cReadReg(kMax3010xI2cAddr, 0x00, status, 2);
            g_int1.store(status[0], std::memory_order_relaxed);
            g_int2.store(status[1], std::memory_order_relaxed);

            // Periodic chip-config CRC readback. 3 small I²C reads
            // (~1 ms total at 100 kHz) every kCfgCrcPeriodMs. Fires
            // *after* the per-edge stats publish so a refresh that
            // runs slow can't push handleInterrupt past the next
            // sample period. Failure leaves the previous CRC value
            // intact (i2cErrTotal ticks via the driver, surfacing
            // the failure to the host without changing cfg_crc).
            if (absolute_time_diff_us(get_absolute_time(), nextCfgCrc) <= 0)
            {
                std::uint16_t crc = 0;
                if (s_sensor.readbackCfgCrc16(crc) == 0)
                {
                    // Two-step publish: store the CRC first, then
                    // bump the counter with release semantics so a
                    // host that sees `cfg_crc_n > 0` is guaranteed to
                    // see the matching `cfg_crc` value (within the
                    // monotonic-counter validity window).
                    g_drvCfgCrc.store(crc, std::memory_order_relaxed);
                    g_drvCfgCrcReadbacks.store(
                        s_sensor.stats().cfgCrcReadbacks,
                        std::memory_order_release);
                }
                nextCfgCrc = make_timeout_time_ms(kCfgCrcPeriodMs);
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

    // Register the GPIO IRQ on core0 BEFORE launching core1. The
    // pico-sdk binds gpio_set_irq_enabled_with_callback's trampoline
    // to the calling core's IRQ slot; we want it on core0 so that
    // the trampoline's `sio_hw->fifo_wr` push targets core0's TX
    // FIFO, which is core1's RX FIFO — where waitForInterrupt() is
    // blocked. Doing this on core1 would route the wake to core0
    // (silent black hole), deadlocking the drain loop.
    PicoIntPin::init(kPinInt);

    multicore_launch_core1(&core1_entry);

    // Wait for core1 to stand up before we start emitting alive
    // frames; otherwise the first one races sensor.configure().
    while (!g_core1Ready.load(std::memory_order_acquire))
    {
        sleep_ms(5);
    }

    // Surface watchdog-induced reboots BEFORE clearing the cause
    // bit so the host can correlate "device just came back" with a
    // hang in the previous run. Must be called before the first
    // watchdog_enable() of this boot.
    if (watchdog_caused_reboot())
    {
        StatusLog::warn("rebooted_by_watchdog",
                        "previous run hung; main loop did not pet "
                        "the WDT within timeout");
    }

    StatusLog::info("ready", "build=%s", OXINODE_BUILD_VERSION);

    // ── OLED dashboard ─────────────────────────────────────────
    // Optional UI: if init() fails (panel unplugged, wiring fault),
    // log it once and carry on — the JSON-Lines link is the source
    // of truth and must keep flowing regardless of the OLED.
    static PicoSsd1306Bus s_oledBus(PicoSsd1306Bus::Config{
        /*i2cIndex*/ 1,
        /*sdaPin  */ kPinOledSda,
        /*sclPin  */ kPinOledScl,
        /*freqHz  */ kI2cOledFreqHz,
    });
    s_oledBus.init();
    static Ssd1306 s_oled(s_oledBus, kOledI2cAddr);
    bool oledOk = false;
    {
        const int rc = s_oled.init();
        if (rc != 0)
        {
            StatusLog::warn("oled_init_failed", "rc=%d", rc);
        }
        else
        {
            // Render an "armed, no signal yet" splash so the user has
            // visual confirmation the panel is alive before the first
            // valid HR/SpO2 reading arrives (~6 s of finger contact).
            s_oled.drawDashboard(0u, 0u, OXINODE_BUILD_VERSION);
            const int frc = s_oled.flush();
            if (frc != 0)
            {
                StatusLog::warn("oled_flush_failed", "rc=%d", frc);
            }
            else
            {
                oledOk = true;
            }
        }
    }

    // ── Stage D: hardware watchdog ─────────────────────────────
    // Enabled only AFTER core1 has signalled ready — we want the
    // boot path itself to run unsupervised so that a configure()
    // crash leaves the device hung-and-reflashable rather than
    // boot-looping. The 8-second timeout (the SDK's effective
    // ceiling for `watchdog_enable`) is comfortably > 10× the
    // worst-case main-loop iteration including the 1 Hz alive-
    // frame snprintf and TinyUSB enqueue.
    //
    // CRITICAL: watchdog_update() below is *unconditional*. Do
    // not gate it on application-level state (samples flowing,
    // ring drained, host responsive, …). That's what the soft
    // liveness fault flags are for (FAULT_STAGNANT_*); they
    // surface the issue without rebooting. The watchdog exists
    // for genuine control-flow lockup — HardFault, infinite
    // loop, I²C bus-stuck-low — where seconds pass with literally
    // zero progress on the main loop.
    constexpr std::uint32_t kWatchdogTimeoutMs = 8000;
    watchdog_enable(kWatchdogTimeoutMs, /*pause_on_debug=*/true);

    // Stage C: per-tick deltas drive level-triggered fault flags.
    // We carry forward the previous frame's values and a stagnation
    // hysteresis (set after 3 s of zero advance, cleared on first
    // advance). The supervisor here NEVER reboots — that's the
    // hardware watchdog's job, configured separately in Stage D.
    std::uint32_t prevSamplesDrained  = 0;
    std::uint32_t prevSamplesConsumed = 0;
    std::uint32_t prevChipOvf         = 0;
    std::uint32_t prevPwrRdy          = 0;
    std::uint32_t prevAlcOvf          = 0;
    std::uint32_t prevI2cErr          = 0;
    std::uint32_t prevRingDrops       = 0;
    std::uint32_t prevDspOverbudget   = 0;
    std::uint32_t producerStaleSec    = 0;
    std::uint32_t consumerStaleSec    = 0;

    constexpr std::uint32_t kStagnantThresholdSec = 3;
    constexpr std::uint32_t kBackpressureHwm      =
        (decltype(g_ring)::capacity() * 3) / 4;   // 75 % of 64 = 48

    absolute_time_t nextAlive = make_timeout_time_ms(1000);
    for (;;)
    {
        // Stage D: pet the watchdog. Unconditional, first-thing-in-
        // the-loop. Main-loop liveness is the only signal that
        // matters here. See the long comment above watchdog_enable().
        watchdog_update();

        // Stage B: drain the SPSC ring as far as it'll go, formatting
        // one JSON line per sample. The producer (core1) refilled it
        // since our last tick; this is where USB writes actually
        // happen now.
        drainRingToUsb(link);

        // Cheap and frequent: poll host CDC for control commands.
        // The repeating timer in UsbCdcLink::init() keeps tud_task()
        // serviced; this loop doesn't need to.
        link.pollHostInput();

        if (absolute_time_diff_us(get_absolute_time(), nextAlive) <= 0)
        {
            UsbCdcLink::AliveStats a{};
            a.tMs              = to_ms_since_boot(get_absolute_time());
            a.edges            = PicoIntPin::edgeCount();
            a.hr               = g_lastHr.load(std::memory_order_relaxed);
            a.spo2             = g_lastSpo2.load(std::memory_order_relaxed);
            a.probeRc          = g_probeRc.load(std::memory_order_relaxed);
            a.configureRc      = g_configureRc.load(std::memory_order_relaxed);
            a.int1             = g_int1.load(std::memory_order_relaxed);
            a.int2             = g_int2.load(std::memory_order_relaxed);
            a.lastDrainRc      = g_lastDrainRc.load(std::memory_order_relaxed);

            a.samplesDrained   = g_drvSamplesDrained.load(std::memory_order_relaxed);
            a.chipOvf          = g_drvChipOvf       .load(std::memory_order_relaxed);
            a.pwrRdy           = g_drvPwrRdy        .load(std::memory_order_relaxed);
            a.alcOvf           = g_drvAlcOvf        .load(std::memory_order_relaxed);
            a.i2cErr           = g_drvI2cErr        .load(std::memory_order_relaxed);

            a.samplesConsumed  = g_samplesConsumed   .load(std::memory_order_relaxed);
            a.ringDrops        = g_ringDrops         .load(std::memory_order_relaxed);
            a.ringHwm          = g_ringHwm           .load(std::memory_order_relaxed);
            a.dspUsMax         = g_dspUsMax          .load(std::memory_order_relaxed);
            a.dspOverbudget    = g_dspOverbudgetTotal.load(std::memory_order_relaxed);

            // ── compute fault_flags from this-vs-prev tick ────────
            std::uint32_t flags = FAULT_NONE;
            if (a.pwrRdy        > prevPwrRdy)        { flags |= FAULT_BROWNOUT; }
            if (a.alcOvf        > prevAlcOvf)        { flags |= FAULT_ALC_DEGRADED; }
            if (a.i2cErr        > prevI2cErr)        { flags |= FAULT_I2C_ERR; }
            if (a.ringDrops     > prevRingDrops)     { flags |= FAULT_RING_DROPS; }
            if (a.dspOverbudget > prevDspOverbudget) { flags |= FAULT_DSP_OVERBUDGET; }
            if (a.ringHwm       >= kBackpressureHwm) { flags |= FAULT_BACKPRESSURE; }

            // Stagnation: producer or consumer stuck for ≥3 ticks.
            producerStaleSec = (a.samplesDrained  == prevSamplesDrained)  ? producerStaleSec + 1 : 0;
            consumerStaleSec = (a.samplesConsumed == prevSamplesConsumed) ? consumerStaleSec + 1 : 0;
            if (producerStaleSec >= kStagnantThresholdSec)
            {
                flags |= FAULT_STAGNANT_PRODUCER;
            }
            // Consumer-stale only counts if producer is *advancing* —
            // if the producer is dead too, that's already covered by
            // the producer flag, no need to double-flag.
            if (consumerStaleSec >= kStagnantThresholdSec &&
                producerStaleSec == 0)
            {
                flags |= FAULT_STAGNANT_CONSUMER;
            }
            a.faultFlags = flags;
            // Read the validity counter with acquire so the matching
            // `cfg_crc` value (paired by core1's release on store) is
            // guaranteed visible.
            a.cfgCrcReadbacks = g_drvCfgCrcReadbacks.load(std::memory_order_acquire);
            a.cfgCrc          = g_drvCfgCrc.load(std::memory_order_relaxed);

            link.writeAlive(a);

            // OLED dashboard: refreshed at the same 1 Hz cadence as
            // the alive frame so the panel always reflects the most
            // recent telemetry the host sees. We render even when
            // bpm/spo2 are zero (driver convention for "not yet
            // valid") so the user sees "---" rather than a stale
            // last-finger reading.
            if (oledOk)
            {
                const std::uint8_t bpm  = (a.hr   < 0) ? 0u : static_cast<std::uint8_t>(a.hr);
                const std::uint8_t spo2 = (a.spo2 < 0) ? 0u : static_cast<std::uint8_t>(a.spo2);
                s_oled.drawDashboard(bpm, spo2, OXINODE_BUILD_VERSION);
                const int rc = s_oled.flush();
                if (rc != 0)
                {
                    // One-shot warn so we don't spam the link if a wire
                    // came loose mid-run; further failures are silent.
                    static bool s_oledWarned = false;
                    if (!s_oledWarned)
                    {
                        StatusLog::warn("oled_flush_failed", "rc=%d", rc);
                        s_oledWarned = true;
                    }
                }
            }

            prevSamplesDrained  = a.samplesDrained;
            prevSamplesConsumed = a.samplesConsumed;
            prevChipOvf         = a.chipOvf;
            prevPwrRdy          = a.pwrRdy;
            prevAlcOvf          = a.alcOvf;
            prevI2cErr          = a.i2cErr;
            prevRingDrops       = a.ringDrops;
            prevDspOverbudget   = a.dspOverbudget;
            (void)prevChipOvf;   // currently informational only

            nextAlive = make_timeout_time_ms(1000);
        }

        // Yield briefly. We deliberately *don't* WFE here — at 25 Hz
        // sample rate a missed wake would mean ~40 ms of latency
        // before the next sample lands; a short tight-poll keeps the
        // ring drained promptly. The loop body is cheap (~10 µs)
        // when the ring is empty.
        sleep_us(200);
    }
}
