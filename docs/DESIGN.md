# OxiNode Design Decisions

## What this document is

The decision log. Every non-obvious choice in OxiNode lives here as a numbered
entry with the same shape: **Title / Context / Decision / Why / Tradeoff /
Alternatives considered**. If you want to know *what* the system does, read
[`ARCHITECTURE.md`](ARCHITECTURE.md). If you want to know *why* it's shaped
that way, you're in the right place.

New decisions append to the bottom; existing entries are not rewritten in place
unless the decision is reversed (in which case the entry is marked
**Superseded** and the replacement entry references it).

---

## D-01 — Portable driver core behind a 4-method HAL

**Context.** The MAX30102 will be driven from two unrelated MCUs (RP2040 and
ESP32-S3) and tested on x86 Linux. The naïve approach is one driver per
platform. This was rejected before any code was written.

**Decision.** A single C++17 driver lives in `lib/max3010x/`. It calls out
through `IHal`, an abstract base with four virtual methods
(`i2cWriteThenRead`, `gpioIntAttach`, `millis`, `delayMs`). Each platform
provides one implementation. Host tests provide a fake.

**Why.** The driver is where every interesting bug lives — register init
order, FIFO pointer arithmetic, DSP edge cases. We want to debug it once,
under gtest, with deterministic input. We do not want to debug it on two MCUs
with two different log paths.

**Tradeoff.** A v-table call on every I²C transaction. Measured cost: ~3 ns
on M0+ at 125 MHz, drowned out by the I²C transaction itself (≈100 µs at
100 kHz). Not measurable.

**Alternatives considered.**

| Option | Why rejected |
|--------|--------------|
| Templated `Max30102<HalT>` | Forces every platform to expose a header `lib/` then `#include`s, which couples build systems. Also turns every gtest run into a recompile of the driver against the fake. |
| Function-pointer table (C-style) | Same indirection cost, worse type safety, no constructor lifecycle. |
| Two parallel drivers, one per MCU | Doubled bug surface; DSP code duplicated; tests run against neither path. |
| ArduinoJson-style `Wire`-as-template | Drags in the Arduino abstraction whether you want it or not. Fine for hobby, wrong for a driver showcase. |

```cpp
// lib/max3010x/include/max3010x/IHal.hpp
class IHal
{
public:
    static constexpr size_t kMaxBurst = 192;   // 32 samples * 6 bytes

    virtual ~IHal() = default;

    [[nodiscard]] virtual int  i2cWriteThenRead(uint8_t addr,
                                                const uint8_t* wr, size_t wlen,
                                                uint8_t* rd,       size_t rlen) = 0;
    virtual void               gpioIntAttach(void (*cb)(void*), void* ctx)      = 0;
    [[nodiscard]] virtual uint32_t millis() const                                = 0;
    virtual void               delayMs(uint32_t ms)                              = 0;
};
```

---

## D-02 — JSON-Lines is the default link mode, binary frames are opt-in

**Context.** The RP2040 firmware needs to push samples to a host over USB-CDC.
The desktop client wants compact binary; a developer with `picocom` wants to
see something readable.

**Decision.** Boot in JSON-Lines (`\n`-terminated UTF-8 JSON objects). Host
sends the literal string `MODE BIN\n` to switch to length-prefixed CRC-16
frames. A `CTRL{cmd=STOP_BIN}` frame switches back. Full grammar in
[`PROTOCOL.md`](PROTOCOL.md).

**Why.** Two audiences, one cable. JSON-Lines means "plug in, run picocom,
see numbers" with zero tooling. Binary mode is for the Python TUI and any
future desktop app that wants ≥200 Hz throughput without a JSON parser in
the hot loop.

**Tradeoff.** A small mode-state machine on the device, and one place where
the protocol is bimodal. Both paths share the same `Sample → record`
pipeline, so the divergence is only at the formatter.

**Alternatives considered.**

| Option | Why rejected |
|--------|--------------|
| Binary only | First-time user sees gibberish in `picocom` and concludes the firmware is broken. Bad demo. |
| JSON only | At 100 Hz with full SpO2 + raw IR/Red, JSON parsing in Python burns more CPU than the chart rendering. |
| MessagePack / CBOR | Adds a parser dependency to both sides. CRC is still needed on top. CRC-framed plain bytes is a smaller, more honest spec. |
| COBS-encoded JSON | Solves the framing problem on a binary channel but doesn't solve the throughput problem; you still pay JSON encode/decode cost. |

---

## D-03 — CRC-16/CCITT-FALSE for the binary frame

**Context.** Binary frames need integrity checking on a USB-CDC channel that
is *almost* lossless but not quite — disconnects, partial writes when the
host's queue is full, and the occasional kernel-level corruption have all
been observed in the wild.

**Decision.** CRC-16/CCITT-FALSE: polynomial `0x1021`, initial value
`0xFFFF`, no input reflection, no output reflection, no XOR-out. Computed
over `LEN || TYPE || PAYLOAD`. Stored little-endian after the payload.

**Why.** It's the same CRC used by XMODEM, MAVLink, and a pile of other
embedded protocols, so off-the-shelf reference code exists in every language
the host might be written in. It's small (16-bit), fast on Cortex-M0+
without a hardware CRC unit, and it catches every single-bit and double-bit
error and every burst of length ≤ 16 bits. That's overkill for USB-CDC,
which is what we want.

**Tradeoff.** 16 bits of overhead per frame (negligible; payload is at
least 14 bytes). Slightly slower to compute than CRC-32 with hardware
support — but the RP2040 doesn't have hardware CRC, and the loop fits in
~30 instructions.

**Alternatives considered.**

| Option | Why rejected |
|--------|--------------|
| No CRC | USB-CDC is *almost* lossless. Almost is not a spec. |
| Fletcher-16 | Faster but has known weaknesses against burst errors. We don't need the speed. |
| CRC-32 | 4 bytes per frame instead of 2; no hardware accel on RP2040; not noticeably stronger for our payload sizes. |
| HMAC | This is integrity, not authenticity. There's no adversary. |

```cpp
// lib/max3010x/include/max3010x/Crc16.hpp
[[nodiscard]] static constexpr uint16_t crc16Ccitt(const uint8_t* data, size_t len)
{
    constexpr uint16_t kPoly = 0x1021;
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; ++b)
        {
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ kPoly)
                                 : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}
```

---

## D-04 — DSP runs on the device, host receives derived values

**Context.** The MAX30102 emits raw 18-bit IR and red ADC samples. The user
wants to see "72 BPM, 98 % SpO2", not a stream of integers. Where does the
DC removal, peak detection, and ratio-of-ratios math run?

**Decision.** All of it runs on the MCU, in `lib/max3010x/`. The wire
format always carries `(t, ir, red, hr, spo2)` — both the raw samples and
the derived values. The host is free to ignore the derived values and
recompute, but does not have to.

**Why.** Three reasons.

1. **Demo value.** Plug into `picocom`, see numbers, walk away convinced.
   No host-side Python required for the basic case.
2. **Phase-2 readiness.** The ESP32-S3 will push samples to a phone over
   Wi-Fi. There is no host-side Python on a phone. The DSP must already be
   on the device for that path to work.
3. **Test surface.** The DSP is the most interesting code we're writing.
   Putting it in `lib/` means it's covered by `host/tests/` against canned
   waveforms — the only place we can run it deterministically.

**Tradeoff.** The MCU spends cycles on DSP. At 50 Hz with the biquad and
peak detector in [`lib/max3010x/src/HrDetector.cpp`](../lib/max3010x/src/HrDetector.cpp)
and the polynomial in [`lib/max3010x/src/Spo2Algo.cpp`](../lib/max3010x/src/Spo2Algo.cpp),
load is well under 1 % on RP2040 core1. We have the cycles.

**Alternatives considered.**

| Option | Why rejected |
|--------|--------------|
| Raw streaming, all DSP host-side | Fails the "phone browser sees a number" requirement for ESP32 phase 2. Also doubles the wire bandwidth for no win. |
| DSP host-side, derived values *also* on device | Two implementations, two test surfaces, one of them never gets exercised. |
| DSP on device, raw not transmitted | Loses debuggability. We want the host to be able to re-derive and validate. |

---

## D-05 — No dynamic allocation in `lib/`

**Context.** The driver core runs on RP2040 (264 KB SRAM total) and on
ESP32-S3 (512 KB internal SRAM). Both have dynamic allocators. Both will
fragment.

**Decision.** No `new`, no `delete`, no `malloc`, no `std::vector`,
`std::string`, `std::map`, `std::function`, or any other container that
allocates. Fixed-capacity types only: arrays, `RingBuffer<T, Capacity>`
where `Capacity` is a power-of-2 template parameter, raw POD structs.
Test code under `host/tests/` may use STL freely.

**Why.** Driver code that allocates becomes driver code that fails to
allocate, and failure-to-allocate paths are the worst things to test.
Banning the operation removes an entire class of bug from the firmware
without any runtime cost. The ban is enforced by the absence of any
`#include <vector>` or `#include <string>` in `lib/max3010x/`.

**Tradeoff.** A few APIs are uglier — `RingBuffer<Sample, 256>` instead of
`std::vector<Sample>`, fixed-size buffers passed by pointer + length
instead of returned. The compile-time size discipline pays off in
predictable RAM use. We can read the static analyzer output and know
exactly how much SRAM the driver will consume.

**Alternatives considered.**

| Option | Why rejected |
|--------|--------------|
| Custom pool allocator | Possible but unjustified — we don't have a use case where ownership transfer is needed across module boundaries. |
| Allow `std::vector` with `reserve()` | Still calls `operator new` somewhere; still fails the "no alloc" CI check. |
| EASTL-style allocators | Big dependency for tiny win. |

---

## D-06 — I²C 100 kHz default, 400 kHz reserved for later

**Context.** The MAX30102 supports I²C standard mode (100 kHz) and fast
mode (400 kHz). The breakouts ship with a 4.7 kΩ pull-up to 1.8 V or
3.3 V depending on variant. Cable lengths are short (≤ 100 mm in the
demo wiring).

**Decision.** v1 firmware initialises I²C0 at 100 kHz on both targets. A
build-time constant `kI2cClockHz` in `firmware/<target>/include/board/pins.hpp`
controls the rate; switching to 400 kHz is one number. v1 release does not
ship the 400 kHz variant.

**Why.** Conservative pull-up sizing on the cheap breakouts is the most
common cause of "intermittent NACK at 400 kHz" complaints in the wild.
Standard mode works on every variant of every breakout I've seen. We
characterise at 100 kHz, then opt into fast mode once we have a known-good
board.

**Tradeoff.** At 100 kHz, draining 32 samples (192 bytes) takes ~17 ms.
This dictates the FIFO_A_FULL watermark we can safely set. At 50 Hz sample
rate, this is comfortable; at higher rates we'll need fast mode.

**Alternatives considered.**

| Option | Why rejected |
|--------|--------------|
| 400 kHz default | Breakout pull-ups are too weak on some clones — failure mode is silent corruption, not a clean NACK. |
| 1 MHz fast mode plus | MAX30102 supports it; breakout layout does not. Out of scope. |
| Software I²C | Not even funny. |

---

## D-07 — Interrupt-driven FIFO drain

**Context.** The MAX30102 has a 32-deep on-chip FIFO and an `INT` pin that
asserts low when occupancy crosses a programmable watermark.

**Decision.** Use the INT pin. Configure `FIFO_A_FULL` to fire at the
watermark we want; ISR signals the data task; task drains in one I²C burst.
Polling mode is not implemented and is not on the roadmap.

**Why.** Polling forces a tradeoff between latency (poll often, waste CPU)
and freshness (poll rarely, samples back up in the FIFO). Interrupt mode
removes the tradeoff: the sensor tells us exactly when it has data ready,
and the burst read amortises the per-sample I²C overhead across an entire
watermark's worth of samples.

**Tradeoff.** Adds a wire and a GPIO. Adds an ISR path. Adds the
complexity of "what if the INT line is stuck low or floating?"
(handled — the task always reads at least once on entry, and if the
overrun bit comes back set we log and reset the FIFO pointers).

**Alternatives considered.**

| Option | Why rejected |
|--------|--------------|
| Pure polled FIFO drain on a 1 ms timer | Wastes ~10× the CPU cycles for the same throughput. |
| One-sample-per-interrupt, watermark = 1 | Higher I²C overhead per sample. We prefer batches. |
| DMA-driven I²C reads | RP2040 PIO can do this; complexity is not justified at 50 Hz. Possible future optimisation. |

---

## D-08 — RP2040 is the prototype, ESP32-S3 is phase 2

**Context.** Both target boards exist on the workbench. Both can run the
driver. Which one is shipped first?

**Decision.** RP2040-Zero firmware is the priority. ESP32-S3-Zero ships
as a build-passing skeleton in v1, with the Wi-Fi/WebSocket path
completed in a phase-2 release.

**Why.** The RP2040 path is shorter to "first sample on screen": USB-CDC
is plug-and-play, no Wi-Fi credential dance, no phone in the loop. It
isolates risk to the driver and the link layer, which is exactly the
risky stuff. Once we know the driver is correct under JSON-Lines and
binary modes against a real sensor, porting to FreeRTOS + WebSocket is
mechanical.

**Tradeoff.** v1 doesn't have the demo of "watch your heart rate on your
phone" yet. That's deliberate — better to ship one well-characterised
path than two half-finished ones.

**Alternatives considered.**

| Option | Why rejected |
|--------|--------------|
| ESP32 first | Wi-Fi setup is a distraction from sensor bring-up; first prototype should isolate the new variable. |
| Both at once | Doubles bring-up time; risks cross-contamination between platform-layer bugs and driver-layer bugs. |

---

## D-09 — MIT license

**Context.** This is a portfolio repo intended to be cloned, read, and
adapted by other engineers. It's also published under my own name and
should not encumber downstream users.

**Decision.** MIT. See [`../LICENSE`](../LICENSE).

**Why.** MIT is the lowest-friction permissive license that still
preserves attribution. Hiring managers, hobbyists, and other engineers
all know what they're getting. No copyleft surprises.

**Tradeoff.** Anyone can take the code into a closed-source product
without contributing back. That's fine — the value of this repo is the
documented design, not exclusivity.

**Alternatives considered.**

| Option | Why rejected |
|--------|--------------|
| Apache-2.0 | Equivalent for our purposes, but the patent-grant clause is overkill for a sensor driver and adds reading friction. |
| BSD-3-Clause | Effectively MIT; less ubiquitous. |
| GPL / LGPL | Would discourage exactly the audience we want to read this. |
| CC-BY | Wrong license family for software. |

---

## D-10 — Maxim ratio-of-ratios polynomial for SpO2, not a learned model

**Context.** SpO2 from a two-wavelength pulse oximeter is an ill-posed
problem. The industry-standard solution is the Beer-Lambert ratio-of-ratios
formulation with an empirical calibration polynomial; modern
research papers report incremental gains from CNN/transformer models on
the raw waveform.

**Decision.** Use the Maxim 4th-order polynomial against the
ratio-of-ratios `R = (AC_red/DC_red) / (AC_ir/DC_ir)`. Coefficients live
as `static constexpr` in
[`lib/max3010x/src/Spo2Algo.cpp`](../lib/max3010x/src/Spo2Algo.cpp). No ML.

**Why.** Three reasons.

1. **Honesty about precision.** The MAX30102 is a hobbyist breakout, not
   an FDA-cleared device. Whatever model we put on top of it is
   bounded by the sensor and skin-contact noise floor, not the model.
   A polynomial is no worse than a CNN on this hardware, and is a lot
   easier to defend.
2. **Embedded-fit.** A 5-coefficient polynomial fits in 20 bytes of
   flash. A CNN does not. We have a hard "no allocations, fits in
   M0+ SRAM" constraint that rules out ML inference frameworks.
3. **Reviewable.** Every reader of `Spo2Algo.cpp` can see the equation,
   trace it back to the Maxim app note (referenced in
   [`DATASHEETS.md`](DATASHEETS.md)), and reproduce the math by hand.

**Tradeoff.** No clever per-user calibration, no motion-artifact
rejection beyond the bandpass. SpO2 reading is "advisory, not medical",
which is true of every hobbyist pulse oximeter and is stated in the
README.

**Alternatives considered.**

| Option | Why rejected |
|--------|--------------|
| TFLite Micro CNN | Doesn't fit on RP2040 with the rest of the firmware; no convincing accuracy gain on this sensor. |
| Lookup table on R | Equivalent to the polynomial in practice, less compact. |
| Live recalibration against a reference | Out of scope for v1; could be added later as a host-side per-user offset. |

```cpp
// lib/max3010x/src/Spo2Algo.cpp  (excerpt)
static constexpr float kSpo2Coeff[5] = {
    -45.060f,    // R^4
    +30.354f,    // R^3
    +94.845f,    // R^2 — Maxim app-note coefficients
    // ...
};

[[nodiscard]] uint8_t Spo2Algo::computePercent(float ratioOfRatios) const
{
    float r  = ratioOfRatios;
    float r2 = r * r;
    float spo2 = kSpo2Coeff[0] * r2 * r2
               + kSpo2Coeff[1] * r2 * r
               + kSpo2Coeff[2] * r2
               + kSpo2Coeff[3] * r
               + kSpo2Coeff[4];

    if (spo2 > 100.0f) { spo2 = 100.0f; }
    if (spo2 <   0.0f) { spo2 =   0.0f; }
    return static_cast<uint8_t>(spo2 + 0.5f);
}
```

---

## D-11 — `pico_stdio_usb` owns the USB descriptor; do not also link `tinyusb_board`

**Context.** The first RP2040 firmware build linked `tinyusb_device` and
`tinyusb_board` in `firmware/rp2040/cmake/oxinode_app.cmake` so the
`UsbCdcLink` could call `tud_cdc_n_*` directly for binary-mode framing,
*and* called `pico_enable_stdio_usb(target 1)` so JSON-Lines could be
emitted via `printf`. The result: the RP2040 booted, but the host USB
stack never enumerated it. `lsusb` saw nothing on the chip's `2e8a:`
address even though the board was clearly powered (the MAX30102 power LED
was lit through the RP2040's 3V3 LDO).

**Decision.** `firmware/rp2040/cmake/oxinode_app.cmake` no longer links
`tinyusb_device` or `tinyusb_board`. `pico_enable_stdio_usb(target 1)` is
the single owner of the USB descriptor. `UsbCdcLink::OXINODE_HAVE_TINYUSB`
is forced to `0`; both JSON-Lines and binary writes go through `printf`
/ `putchar`, which `pico_stdio_usb` plumbs into TinyUSB internally.

**Why.** `pico_stdio_usb` ships its own minimal CDC USB descriptor and a
private `tusb_config.h` that defines `CFG_TUD_CDC=1`. Linking
`tinyusb_board` adds a *second* descriptor — the standalone-app one
intended for cases where the firmware owns USB itself. The two collide:
either the linker resolves them in a way that produces a malformed
configuration, or one descriptor wins and the other's enumeration trap
masks it. Either way, the host side never gets a valid `GET_DESCRIPTOR`
response and gives up. Removing `tinyusb_board` is the only clean fix
short of writing our own custom USB descriptor (which `pico_stdio_usb`
deliberately makes friction-y to discourage).

**Tradeoff.** Direct `tud_cdc_n_*` calls are inaccessible: the
declarations are gated behind a `tusb_config.h` that `pico_stdio_usb`
owns and does not re-export. Concretely, the host control parser
(`MODE BIN\n` → switch the firmware to binary frames) is dead in the
current build. JSON-Lines is the only mode emitted, which covers v1.
A custom USB descriptor — owned by us, dropping `pico_stdio_usb` — is
the unblock when binary mode becomes load-bearing.

**Alternatives considered.**

| Option | Why rejected |
|--------|--------------|
| Keep both, hope link order is right | Tried; result was zero enumeration. Not deterministic across SDK revisions. |
| Use `pico_stdio_usb_v2` (pico-sdk experimental) | API surface is unstable across pico-sdk 1.x → 2.x; not worth the churn for a v1 bring-up. |
| Hand-write a `tusb_config.h` that includes both | Possible but invasive; conflicts with `pico_stdio_usb`'s buffer sizes and endpoints. Defer until binary mode is actually needed. |

The diagnostic that found this: enumeration was silent (no `lsusb` line
at all, even after BOOTSEL → fresh flash → reset). The fix was the
single-line removal of the `tinyusb_device tinyusb_board` link clause.

---

## D-12 — Hybrid IRQ + 50 ms polled FIFO drain  *(superseded by [D-14](#d-14--switched-back-to-irq-only-drain-after-bring-up))*

> **Status as of 2026-04-26:** the polled fallback was a bring-up
> safety net. With the chip's IRQ wiring verified and PPG_RDY enabled
> (so INT fires every output sample, ~25 Hz), the polled wake was
> redundant. Drain is now pure-IRQ. See D-14 for the rationale.

**Context.** The MAX30102 INT line is open-drain active-low. The
canonical embedded path is to wire it to a GPIO, arm a falling-edge
IRQ, and drain the FIFO from the ISR-bottom-half on every edge. The
RP2040 firmware does exactly this — `PicoIntPin::init()` arms a
falling-edge IRQ on `GP6`, the ISR bumps an atomic counter and pushes
a wake token into the multicore FIFO, core1 blocks on
`multicore_fifo_pop_blocking()` and calls `Max30102::handleInterrupt()`
on wake-up. This works.

It also has one failure mode that's painful to debug from a working
firmware: any wiring fault on `INT`, any silkscreen mislabel, any
edge-triggered IRQ that didn't arm right, manifests as "the alive
frame's `edges` field stays at 0 and no samples ever stream". The
firmware looks alive (USB-CDC up, JSON banner emitted) but the data
path is silent.

**Decision.** core1 still waits on the multicore FIFO, but with a
50 ms timeout via the new `PicoIntPin::waitForInterruptOrTimeout()`.
Whether the wait returns because of an edge or a timeout, core1 then
calls `Max30102::handleInterrupt()` — which returns 0 cheaply when
nothing is pending, or reads any queued FIFO entries when there is.

**Why.**

1. **Survives INT-line wiring faults.** The firmware reaches usable
   data even if `INT` is disconnected entirely. Diagnostic alive
   frames make the IRQ-vs-polled distinction visible (`edges` rising
   = IRQ healthy; `edges` stuck = polled drain is carrying us).
2. **Survives IRQ-routing regressions.** pico-sdk routes GPIO IRQs to
   the core that armed them, but the exact rules around
   `multicore_fifo_pop_blocking` and `gpio_set_irq_callback` are
   subtle enough that small SDK changes can break them silently.
3. **The cost is negligible.** One INTR_STATUS read per 50 ms is
   ~50 µs of I²C bus time at 100 kHz — 0.1 % bus utilisation.

**Tradeoff.** If `INT` is healthy, the IRQ path drains the FIFO
within a sample-period (10 ms) of the edge. The polled path adds
up to 50 ms of latency *if* the IRQ is silent, which only happens
during a fault. Under no realistic workload is the polled path the
hot drain.

**Alternatives considered.**

| Option | Why rejected |
|--------|--------------|
| IRQ-only | Already shown to fail-silent during the bring-up — needed a probe before we knew the IRQ was alive. |
| Polled-only | Adds 50 ms of consistent latency in the healthy case; wastes sample-rate headroom. |
| 50 ms polled with fallback to IRQ | Inverted version of what we shipped. Same correctness, but confusing — IRQ should be the fast path. |
| `tud_task`-driven drain | Couples the data path to USB. Defeats the point of having a separate sample-drain core. |

```cpp
// firmware/rp2040/apps/oxinode/main.cpp — core1 loop
for (;;)
{
    (void)PicoIntPin::waitForInterruptOrTimeout(50);
    const int n = s_sensor.handleInterrupt();
    g_lastDrainRc.store(n, std::memory_order_relaxed);
    // ... snapshot INTR_STATUS_{1,2} for the alive frame
}
```

The polled drain saved the bring-up: the canonical IRQ path *was*
working, but until we had alive frames carrying live `edges` /
`int1` / `probe` / `cfg` values we couldn't tell that from a wiring
fault. Hybrid-by-default keeps the next bring-up's diagnostic loop
short.

---

## D-13 — Replaced homegrown HR detector with Maxim's `algorithm.cpp` port; SpO2 uses AN6845 calibrated quadratic

**Context.** Initial bring-up shipped a streaming `HrDetector` of my
own design — a single-pole DC remover, 4-tap MA, asymmetric envelope
tracker (α=0.999/0.001), threshold-crossing peak detector. On real
hardware the HR field stuck at `-1` (signal too noisy at AVG_1) or
clamped to `31` BPM (envelope settling time mismatched the post-AVG_4
sample rate). SpO2 worked fine. After the user supplied four extra
Maxim docs (UG6409 — *Recommended Configurations and Operating
Profiles for MAX30101/MAX30102 EV Kits*; AN6845 — *Guidelines for
SpO2 Measurement*; AN6410 — *SNR as a Quantitative Measure*; AN6433
— *Penetration Depth vs. Wavelength*), it became clear the homegrown
detector was solving the wrong problem in the wrong way.

**Decision.** Rewrite both DSP paths to follow the published Maxim
references exactly:

1. **HR detector.** Direct port of the MAXREFDES117# reference
   (`algorithm.cpp::maxim_heart_rate_and_oxygen_saturation` +
   `maxim_find_peaks`), described step-by-step in UG6409 §"Heart-Rate
   Post-Processing" (p.29-30). Buffer 100 samples (4 s @ 25 Hz) —
   subtract mean — invert (so peak-finder finds systolic upstrokes,
   which are *valleys* in the IR-ADC trace because more blood absorbs
   more light) — 4-tap moving-average smoother — `find_peaks` above
   a height floor of 30 ADC counts, separated by ≥ 4 samples (≈ 0.16 s
   = 375 BPM ceiling) — BPM = (Fs · 60) / mean_peak_interval. Recompute
   once per second; cache the BPM between recomputes.
2. **SpO2 quadratic.** Replaced the linear `110 − 25R` placeholder
   with AN6845 Table 1 (p.13) — Maxim's published calibrated
   coefficients for the MAX30101 / MAX30102 with no optical shield:
   `SpO2 = 1.5958422·R² − 34.6596622·R + 112.6898759`. Same R from
   the rolling-window AC RMS / DC mean computation; only the curve
   changes.
3. **Chip config alignment.** UG6409 §"Recommended Practices"
   (p.26-27) calls for `SMP_AVE = 4` so the chip outputs 25 Hz
   denoised samples — the rate the reference HR algorithm assumes.
   We had `AVG_1` because earlier I'd tried to dodge the envelope-
   tracker mistuning by feeding it 100 Hz; the real fix is the
   right algorithm at the right rate. AN6845 step 8 (p.9) wants DC
   ≥ 150 K counts on a finger device; `irLedPa = redLedPa = 0x3F`
   (~12.5 mA) lands DC at ≈240 K (IR) / ≈207 K (RED).

**Why.** UG6409 page 9 — "An SpO2 algorithm used with the MAX30102
output signal can compensate for the associated SpO2 error" — and
the LED-Driver paragraph below it — "to allow the algorithm to
optimize SpO2 and HR accuracy" — make it explicit: the datasheet
hands the DSP off to the application code, and the algorithm Maxim
endorses is the one shipped with the MAXREFDES117# reference design.
That is the same code SparkFun bundles in their MAX3010x library
(`spo2_algorithm.cpp` is a verbatim copy with the original
copyright header). Millions of users get plausible HR readings out
of MAX30102 modules because they are running this exact algorithm
with the exact chip configuration UG6409 prescribes.

**Tradeoff.** We give up the streaming nature of the old detector —
BPM only updates once per second now, and the first reading is
delayed by the 4-second buffer fill. In exchange:

- HR locks to a stable resting BPM on a fingertip (verified on
  hardware: 78 BPM steady, see `git log` after this commit).
- The DSP is bit-faithful to a reference that's been validated
  against thousands of users in the SparkFun ecosystem.
- All-integer math — RP2040 Cortex-M0+ has no FPU, so the per-sample
  float pipeline of the old detector was secretly software-emulated
  every sample at 25/100 Hz.

**Empirical surprise — UG6409's "¼ to ¾ FS" rule conflicts with
HR-detector SNR.** UG6409 p.19 wants DC + AC sitting between ¼ FS
(65 K) and ¾ FS (197 K) for SpO2 calibration accuracy. Our `0x3F`
LED PA puts IR DC at 240 K = 91 % FS, comfortably above the
doctrinal ceiling. I tried `0x30` (~9.6 mA), which landed DC at
183 K (70 % FS) — squarely in the recommended window. **HR detection
got worse**: scattered across 11 unique values 48–166 BPM with no
clear lock, vs. clean 78 BPM at `0x3F`. The smaller AC swing (7.8 K
vs 12.5 K p-p) starves the find_peaks SNR. UG6409's headroom rule is
intended for production wearables that must work across diverse skin
tones and motion conditions; for benchtop bring-up with a single
calm finger, more current is unambiguously better. We never saturate
(IR peak 243 K vs FS 262 K, 7 % margin), so the doctrine doesn't
bind. **Documented `0x3F` as the verified-good value with a long
comment in `Max30102::Config` so a future cleanup pass doesn't
revert it back to the doctrinal target.**

**Alternatives considered.**

| Option | Why rejected |
|--------|--------------|
| Keep homegrown envelope tracker, retune α. | Three tuning knobs (α-up, α-down, peak-fraction) interacting non-obviously. Even after Maxim docs landed, no principled way to set them. |
| Port SparkFun's PBA (`checkForBeat`, `heartRate.cpp`). | Same author, same Maxim copyright, but it's the *streaming* zero-crossing variant. UG6409 publishes the buffered `find_peaks` variant. We chose the one Maxim documents directly. |
| Port `algorithm_by_RF.cpp` (autocorrelation). | More robust to motion artifacts. Worth doing later if the simpler port underperforms in the field, but it's not what the docs walk through. Leave as a v2 candidate. |
| Tune `HrDetector` constants to the new 25 Hz rate, keep envelope approach. | Even with better tuning, a streaming envelope-tracker fundamentally cannot match the SNR of a 4-second buffered batch. Moot. |

**Test coverage.** Host gtest grew from 27 → 30 cases:
`HrDetectorTest.SlowSignal40Bpm` (regression for the bradycardia case
the old detector failed on), `BufferNotFullGivesZero` (no false
publication during fill), `HighFrequencyNoiseRejected` (8 Hz noise
on a 60 BPM fundamental still locks). SpO2 tests' expected values
shifted to match AN6845's quadratic (R=0.4 → 99 %, R=0.8 → 86 %,
R=1.4 → 67 %).

---

## D-14 — Switched back to IRQ-only drain after bring-up

**Context.** [D-12](#d-12--hybrid-irq--50-ms-polled-fifo-drain)
shipped the bring-up firmware with a hybrid IRQ + 50 ms polled FIFO
drain. The polled half was insurance: if the GPIO IRQ wiring or the
chip's INT configuration was wrong, the data path would still
advance every 50 ms and the alive frame would surface the diagnostic.
That insurance has done its job — first light, the algorithm
correctness work, and the post-D-13 validation all confirmed the IRQ
path is healthy. The `edges` counter advances ~20–25 times per
second (PPG_RDY at 25 Hz output rate plus occasional A_FULL races),
which means the IRQ path is doing all the real work and the polled
wake almost always finds nothing pending.

**Decision.** Drop the 50 ms polled fallback. core1's drain loop now
calls `PicoIntPin::waitForInterrupt()` (blocking
`multicore_fifo_pop_blocking`) and runs `handleInterrupt()` only on
real wake tokens from the GP6 ISR. One-line change in `main.cpp`:

```cpp
// before: bounded wait
(void)PicoIntPin::waitForInterruptOrTimeout(50);

// after: blocking wait, IRQ-only
(void)PicoIntPin::waitForInterrupt();
```

**Why.** Three reasons:

1. **PPG_RDY is on.** `Max30102::configure()` enables both `A_FULL`
   and `PPG_RDY` in `INTR_ENABLE_1`. PPG_RDY fires once per output
   sample (25 Hz at SR=100 / AVG=4), so the IRQ rate already matches
   the sample rate. The 50 ms polled wake was firing at ~20 Hz and
   doing zero work in the steady state — pure overhead.
2. **The diagnostic surface is intact without it.** The alive frame
   still publishes `edges`, `int1`, `int2`, `probe`, `cfg`, `drain`
   on a 1 Hz timer driven from core0. A wedged sensor manifests as
   `edges` flatlining or `int1` stuck non-zero — same diagnosis path
   as before, just slower.
3. **Cleaner semantics.** With the polled wake gone, every entry to
   `handleInterrupt()` corresponds to a real chip interrupt. The
   "early-out on no FIFO event" branch (`Max30102.cpp:185`) becomes
   the no-op path for races (chip cleared the latch between ISR
   posting the wake and core1 reading INTR_STATUS_1) rather than the
   normal-case path — which is what the original Maxim reference code
   assumes.

**Tradeoff.** A wiring fault that breaks INT *after* a confirmed
healthy boot now hangs core1 forever instead of being papered over by
the 50 ms wake. core0's alive frame still streams, the host still sees
"edges flat / drain not advancing" within 1 second, but the `t`
field of sample lines stops advancing because no samples are being
drained. This is the *correct* failure mode — silent fall-back to
polling masked the real problem on the bench.

**Alternatives considered.**

| Option | Why rejected |
|--------|--------------|
| Keep the hybrid as a `OXINODE_DRAIN_HYBRID` compile-time toggle. | YAGNI. If we ever need it back during another bring-up, the change is one line in `main.cpp` and a `git revert`. The toggle would just decay. |
| Keep polled wake but extend timeout to 1 s (purely a watchdog). | Still fires once per sample at 25 Hz — pointless. The watchdog is core0's alive frame. |
| Disable PPG_RDY, leave A_FULL only. | Burns the IRQ rate down to ~A_FULL frequency. No correctness benefit; trades IRQ overhead for FIFO depth utilisation. The chip handles 25 IRQs/sec without any fuss. |

**Follow-up: known footgun in `Config::fifoAlmostFullThreshold`.**
While reading the code to answer "when does INT fire", I noticed
`Max30102.cpp:97` masks the threshold value with `& 0x0F`:

```cpp
static_cast<uint8_t>(cfg.fifoAlmostFullThreshold & 0x0F);
```

The default `Config::fifoAlmostFullThreshold = 17` → masked to 1.
Per the MAX30102 datasheet rev 1, FIFO_A_FULL[3:0] = 1 means
"interrupt fires when 1 free space remains" = 31 unread entries.
UG6409 page 27 reads the same field as "interrupt fires when there
is 1 sample in FIFO" — the docs disagree. Either way our intended
semantics ("fire when ~17 entries are unread") is not what the chip
sees. **It does not matter today** because PPG_RDY is enabled and
fires every sample anyway, so A_FULL essentially never gets a chance
to assert before INTR_STATUS_1 is cleared. **It will matter** if a
future change disables PPG_RDY for power-savings or batches reads.
Tracked as a follow-up; cleanest fix is to change the field type to
something self-documenting (`enum class FifoAFull : uint8_t { ... }`)
and assert the value fits in 4 bits.

---

## D-15 — Producer/consumer split, observability counters, and a deliberately-narrow watchdog

**Context.** [D-14](#d-14--switched-back-to-irq-only-drain-after-bring-up)
left the firmware with a clean IRQ-driven drain, but the work *after*
the drain — DSP, JSON encoding, USB enqueue, and observer fan-out —
all ran in the same core1 context. Three concrete issues followed:

1. **Back-pressure leakage.** A slow USB host (pico CDC TX queue full)
   blocked `tud_cdc_write` inside the drain path. Drain stalled until
   USB drained. The chip's FIFO would silently overflow if the stall
   was long enough, and we had no way to count it.
2. **Inattentive ISR dispatch.** `Max30102::handleInterrupt()` only
   checked `(A_FULL | PPG_RDY)`. **PWR_RDY was silently ignored** —
   if the chip browned out (flaky USB cable), it came back up
   un-configured and the firmware kept reading garbage. **ALC_OVF
   was silently ignored** — bright ambient light degraded samples
   with no fingerprint. The `OVF_COUNTER` was read but never
   surfaced.
3. **No "tomorrow someone breaks it" alarm.** A future contributor
   could regress the consumer-side latency by a factor of 5 and the
   firmware would back-pressure silently.

**Decision.** Land four small commits as one logical change:

- **Stage A — Smart ISR dispatch + PWR_RDY recovery.**
  `Max30102::handleInterrupt()` now reads `INTR_STATUS_1` and
  dispatches per-flag: PWR_RDY runs `configure(m_cfg)` to re-init
  after brownout (which itself resets DSP state), ALC_OVF
  increments a counter and continues, A_FULL/PPG_RDY drains as
  before. `OVF_COUNTER` accumulates monotonically into
  `Stats::chipOvfTotal`. The driver exposes `Max30102::Stats stats()`
  with `samplesDrained / chipOvfTotal / pwrRdyEvents / alcOvfEvents
  / i2cErrTotal / lastInt1 / lastInt2`. **Counters are monotonic
  u32; never reset on read** — host computes deltas. (See the round-
  table in conversation; the reliability expert was firm on this.)
  Seven new gtests cover each dispatch path.

- **Stage B — SPSC ring + producer/consumer split.**
  Added `firmware/rp2040/apps/oxinode/inc/SampleRing.hpp`: a
  64-slot lock-free single-producer / single-consumer ring
  (12 B/sample × 64 = 768 B). Power-of-2 capacity with bit-mask
  indexing; head/tail are monotonically-increasing `u32` so
  modular subtraction gives depth without empty/full ambiguity.
  Acquire/release on the indices — no mutex, no `__disable_irq`,
  no critical section. `RingPushObserver` (core1) replaces the
  old `LinkObserver`: it pushes decoded samples into the ring and
  bumps `g_ringDrops` on full. core0's main loop drains the ring
  per iteration, formats one JSON line per sample, and enqueues to
  TinyUSB. **A slow USB host can no longer back-pressure the drain
  path** — at worst the ring fills, drops are counted, surfaced.

- **Stage C — Extended alive frame + soft liveness.**
  `UsbCdcLink::writeAlive` takes a struct (`AliveStats`) carrying
  every counter from Stages A + B plus a `fault_flags` u32 bitmap
  (`FaultFlags.hpp`). core0 carries one tick of history and sets
  flags level-triggered: `FAULT_BROWNOUT` /
  `FAULT_ALC_DEGRADED` / `FAULT_I2C_ERR` / `FAULT_RING_DROPS` /
  `FAULT_BACKPRESSURE` (ring HWM ≥ 75 %) / `FAULT_DSP_OVERBUDGET`
  (`time_us_64()`-measured per-sample consumer body > 28 ms)
  / `FAULT_STAGNANT_PRODUCER` / `FAULT_STAGNANT_CONSUMER` (no
  advance for ≥ 3 s). **The supervisor never reboots; it only
  reports.** The host (or `scripts/burn-in.sh`) decides what to
  do with the flags.

- **Stage D — Hardware watchdog + CI gate.**
  RP2040 hardware watchdog, **8 s timeout**, enabled only after
  core1 signals ready (so a configure-time crash leaves the
  device hung-and-reflashable rather than boot-looping). Petted
  *unconditionally* from the top of core0's main loop:

  ```cpp
  for (;;) {
      watchdog_update();          // first thing — main-loop liveness only
      drainRingToUsb(link);
      link.pollHostInput();
      // ... 1 Hz alive frame ...
      sleep_us(200);
  }
  ```

  The `do-not-gate-this-on-app-state` discipline is the most
  important rule; it's spelled out in a long comment next to the
  `watchdog_enable()` call. Application-level health (samples
  flowing, ring drained, host responsive) belongs in the soft
  liveness flags from Stage C, *not* in the petting condition,
  because a transient miss must not reboot a working device.
  `watchdog_caused_reboot()` runs once at boot and emits a
  `StatusLog::warn("rebooted_by_watchdog", ...)` line so the host
  can correlate.

  `scripts/burn-in.sh` is the host-side gate: capture 60 s of
  alive frames, assert `chip_ovf == 0 && ring_drops == 0 &&
  dsp_overbudget == 0 && i2c_err == 0 && fault_flags == 0 &&
  pwr_rdy ≤ 1`. The thresholds defined in firmware (`kDspBudgetUs`)
  are the source of truth — the script is a witness, not an oracle.

**Why.** Three interlocking reasons:

1. The textbook embedded producer/consumer pattern, applied
   honestly. No RTOS — a 2-task system on a dual-core M0+ doesn't
   need one (the bare-metal expert called FreeRTOS for two tasks
   "embarrassing"; the RTOS expert agreed and recommended saving
   it for when BLE or display lands).
2. **Observability is the alarm**, not the watchdog. The watchdog
   is a last-resort recovery mechanism. The actual signal a
   regression happened is the alive-frame counters going non-zero
   and `scripts/burn-in.sh` failing in CI.
3. **PWR_RDY recovery is non-negotiable.** Field deployments will
   see brownouts (USB cable wiggles, marginal 5 V supply). A pulse-
   oximeter that silently keeps reporting after a brownout is
   strictly worse than one that says "hold on, recovering" and
   reinits.

**Tradeoff.** The change adds ~600 LoC across seven files, four
new monotonic counters per stage, and one host-side script. Per-
sample cost on core0 is one `time_us_64()` pair (~10 cycles) plus
two `compare_exchange_weak` retries on the saturating max — total
~30 cycles, dwarfed by the JSON `snprintf`. SPSC ring footprint
is 768 B. RAM impact: **~1 KB** (ring + atomics + per-tick
deltas). Code-size impact: ~3 KB.

Tests grew from 30 → 37 host gtests (Stage A added 7 dispatch-path
tests against `FakeI2cHal`).

**Alternatives considered.**

| Option | Why rejected |
|--------|--------------|
| FreeRTOS port (drain task + consumer task + queue). | ~6 KB code + scheduler overhead, devicetree noise, second toolchain. RTOS expert agreed: "for a 2-thread RP2040 problem, embarrassing." |
| Watchdog gated on "samples flowing AND consumer advanced". | Boot-loop trap. A 600 ms DSP recompute spike during normal operation tries to reboot a working device. The user explicitly called this out: "watchdog should be configured very carefully, else we might end up in a state where it loops watchdogs out." |
| `xStreamBuffer`-style dynamic-size ring. | Variable-size needs a heap or a fancy allocator; samples are fixed 12 B. Hand-rolled SPSC is cheaper and matches the data model. |
| Move DSP to core0 too (driver becomes pure FIFO drain, observers vanish from `Max30102`). | Bigger refactor (driver public API change), no clearly proven win on hardware. Current setup keeps DSP in driver + observer chain on core1; the back-pressure problem was solved by moving USB to core0. Revisit if DSP cost ever becomes a bottleneck. |
| Sticky fault flags (latch until host clears). | Conflates "currently degraded" with "ever degraded". Level-triggered is simpler, and the burn-in script samples the last frame which is enough to catch any never-cleared flag. |
| `__scratch_x` core1-local SRAM for the ring. | Saves a couple of cross-bank reads on the consumer side; the bare-metal expert recommended it. Worth doing as a follow-up; not worth blocking the main change on. |

**Follow-ups (tracked, not done):**

- Move `g_ring` to `__scratch_x` core1-local SRAM with a section
  attribute. Cleaner cross-core memory layout.
- `cfg_crc` extended alive frame at 10 s — readback all written
  config registers, CRC them, surface in `cfg_crc` field.
  Catches "chip silently rebooted with the wrong config" beyond
  what PWR_RDY covers (rare but possible on a flaky bus).
- `Config::fifoAlmostFullThreshold` 4-bit truncation footgun
  (D-14 follow-up). Fix the field type to `enum class FifoAFull
  : uint8_t { ... }` with a `static_assert` on size.
- Move plotter (`host/tools/plot_live.py`) and visualizer panel
  redesign (mean / min / max overlays, perfusion index, lock
  pill — see the medical/UX/data-viz round-table in conversation).

---

## D-16 — GPIO IRQ trampoline must run on the consumer's *peer* core

**Context.** D-15 Stage A made core1 the producer (owns the I²C bus,
runs the sensor driver) and core0 the consumer (drains the SPSC ring,
formats USB JSON-Lines). For wake / sleep, core1 blocks on
`multicore_fifo_pop_blocking()` and the GPIO IRQ trampoline pushes a
wake token via `sio_hw->fifo_wr`. D-14 declared the IRQ-only path
correct and dropped the D-12 polled fallback.

The first hardware run of that combined design (2026-04-26 bench)
wedged: exactly one edge fired at boot, then no more, with
`smpl_d=0` indefinitely and `fault_flags=64` (STAGNANT_PRODUCER).
Restoring a 100 ms safety poll temporarily proved the chip itself
was producing samples normally — the IRQ pipeline was the bug.

**Decision.** Register the GPIO IRQ from **core0**, not core1. Call
`PicoIntPin::init()` from `main()` *before* `multicore_launch_core1()`,
not from inside `core1_entry()`.

**Why.** The RP2040 SIO inter-core FIFO is **directional per core**:
`sio_hw->fifo_wr` on the calling core enqueues to *the other core's*
RX FIFO. `multicore_fifo_pop_blocking()` reads from *the calling
core's own* RX. So:

- IRQ trampoline on core0 → push targets core0's TX = **core1's RX** → `pop_blocking` on core1 receives. ✓
- IRQ trampoline on core1 → push targets core1's TX = **core0's RX** → `pop_blocking` on core1 reads its own RX, which no one writes to → blocks forever. ✗

`gpio_set_irq_enabled_with_callback` binds the trampoline to the
calling core's IRQ vector, so the *call site* of `PicoIntPin::init`
determines which core runs the ISR. Moving that one call from
`core1_entry` to `main()` fixes the routing.

The on-bench effect was immediate: `edges` and `samplesDrained` go
1:1, the consumer ring stays at depth 1, and HR/SpO2 update at
sample-period cadence with no fallback poll.

**Tradeoff.** The GPIO-IRQ-on-core0 placement crosses the producer-
on-core1 boundary cosmetically — it looks weird in the call graph.
The justification (peer-core wake routing) is documented at the
call site in `main.cpp` and inside `core1_entry` so future readers
don't undo the fix on principle.

There is also a subtle dependency: `PicoIntPin::init` must be called
*before* `multicore_launch_core1`, but core1 is what calls
`Max30102::configure` which arms the LEDs and starts producing
samples. So at the moment the IRQ becomes live, the chip is still
quiet — exactly the order we want, no missed samples in the gap.

**Alternatives considered.**

| Option | Why rejected |
|--------|--------------|
| Hybrid IRQ + 100 ms poll (revert to D-12 shape). | Masks bugs rather than fixing them. The user explicitly pushed back: "I want IRQ to work … high interrupt rate doesn't mean the CPU shouldn't be able to handle it." At 25 Hz the M0+ has ~5 M cycles between samples; the ISR is a single atomic + a FIFO write — there is no throughput excuse for missing edges. |
| Cross-core spinlock + `__wfe` on core1, `__sev` from core0 ISR. | Reinvents what `multicore_fifo_pop_blocking` already does, just without the routing bug. Once the routing was fixed, the SDK primitive is the right answer. |
| `sem_t` on core1, `sem_release` from core0 ISR. | Slightly heavier, no advantage on a 2-task system. |
| Move I²C bus ownership to core0 too (driver + drain on the same core). | Big refactor, undoes D-15 producer/consumer split. Not justified by a single SDK gotcha. |

**Follow-ups (tracked, not done).**

- Add a host gtest that asserts `PicoIntPin::init` runs on core0.
  Hard to express without cross-compiling — likely a doc-only
  guard in `core1_entry` (a static-assert-ish runtime check that
  `multicore_fifo_wready()` was already exercised once before
  the loop body runs).

---

## D-17 — Median-of-3 output filter on HR

**Context.** The Maxim batch HR algorithm (D-13) recomputes once per
second over the last 4 s of IR data. On a quietly-resting finger it
converges within 4–6 s, then mostly stays stable — but occasionally a
single recompute lands on a noisy peak set and reports a one-cycle
excursion before the next recompute window re-converges.

Bench data, 30 s capture with finger held still
(`/tmp/oxinode-hr-30s.jsonl`, 2026-04-26):

- median = 88 BPM (stable)
- 90 % of readings = 88
- 10 % split among single-second excursions to 68, 93, 115
- each excursion lasts exactly one 1 Hz recompute window before snapping back

The pattern is unambiguous: isolated outliers surrounded by stable
neighbours. SpO2 over the same window: 96 or 97, no excursions.

**Decision.** Add a 3-deep ring of recompute outputs (including the
0-sentinel for "not yet valid") to `HrDetector`. `bpm()` returns the
median of the latest three entries.

```cpp
static constexpr int kBpmMedianN = 3;
uint8_t m_bpmHistory[kBpmMedianN] = {};
int     m_bpmHistoryHead = 0;
// pushOutput(...) writes ring + sets m_bpm = medianOf3(...)
```

**Why.** Median-of-3 squashes any single-tick outlier sandwiched
between two stable readings — exactly the failure pattern. Pushing
the 0-sentinel on invalid recomputes also gives a graceful fadeout
when the finger is removed: after `kBufferSec + kBpmMedianN` = 7 s of
no signal, the ring fully drains and `bpm()` returns 0.

**Tradeoff.** Up to `kBpmMedianN-1` = 2 s of extra latency on a *real*,
sustained HR change. Acceptable for steady-finger SpO2 use. Not
acceptable for HRV / fitness applications, but those want the raw
beat-to-beat intervals anyway, not a debounced display value.

**Alternatives considered.**

| Option | Why rejected |
|--------|--------------|
| Tighten `kBpmMin/kBpmMax` plausibility band. | The spurious values (68, 93, 115) are all firmly inside any sensible adult-resting band. Doesn't filter them. |
| Rolling mean over last N. | Not robust — one 115 BPM excursion drags a 3-sample mean to 90.3, which on uint8 displays as 90 (a wrong but not-obviously-wrong value). Median ignores the outlier entirely. |
| Reject if `|new − last| > 20 %` (rate-of-change limiter). | Latches forever if the very first valid reading happens to be wrong (no recovery path without reset). |
| Smooth at the host (in `plot_live.py`). | Makes the OLED dashboard see un-smoothed values. The smoothing belongs at the source of truth, which is the driver. |

Median-of-3 also has the cleanest invariant: **`bpm()` is always a
real value the algorithm produced at some recompute** — never an
average that nobody actually computed.

---

## D-18 — SSD1306 OLED dashboard on a separate I²C bus

**Context.** Bring-up was working with HR/SpO2 streaming to a host
client over USB-CDC, but a host requirement makes the device feel
incomplete for hands-on demo / wearable use. The user supplied a
0.96" SSD1306 OLED (128×64, I²C, address 0x3C) and wired it to
GP14/GP15.

**Decision.** Add a portable SSD1306 driver (`lib/ssd1306/`) mirroring
the `lib/max3010x/` pattern: own minimal `IBus` interface (write-only
— SSD1306 is fire-and-forget), embedded 5×7 font, 128×64 frame
buffer, `init()` / `clear()` / `drawPixel()` / `drawText()` /
`drawDashboard()` / `flush()`. Run it on **I²C1** (GP14/GP15, 400 kHz),
fully isolated from the MAX30102 on I²C0. Render the OxiNode
dashboard at the same 1 Hz cadence as the alive frame; OLED
init / flush failures log once but never block sensor data.

**Why.**

1. Two I²C peripherals → no bus contention. A 1 KB full-frame OLED
   flush takes ~25 ms at 400 kHz; co-locating it on I²C0 with the
   MAX30102 sample drain would either need bus arbitration logic
   or risk starving the chip's drain.
2. Portable lib (no platform headers, host-testable) keeps the same
   contract as `lib/max3010x`. Re-used the existing `FakeI2cHal`-
   style test pattern with a small `FakeSsd1306Bus`. 11 host gtests
   cover init, framebuffer, text, flush, and dashboard.
3. Optional UI semantics: if the panel is unplugged at boot, the
   firmware logs `oled_init_failed` once and the JSON-Lines link
   to the host keeps flowing. The OLED never gates the sensor path.

**Tradeoff.** RP2040 has only two I²C peripherals; we now use both.
A future feature that needs a third I²C device has to share a bus
with one of the existing two (either by address or by software
arbitration). Acceptable: nothing on the roadmap needs that yet.

**Alternatives considered.**

| Option | Why rejected |
|--------|--------------|
| Share I²C0 with the MAX30102 (different address, same bus). | Adds a 25 ms full-frame burst into the sensor drain timing. Doable, but no benefit — we have a free I²C peripheral. |
| PIO-bit-bang I²C on arbitrary pins. | The user moved wires cleanly to a hardware I²C1 pair, so PIO complexity isn't justified. |
| Bigger / scaled font for "BPM" / "%" digits. | The 5×7 font fits "OxiNode v1.0 / HR : 075 bpm / SpO2:  98 %" cleanly with room for future fields. Bigger digits is a follow-up, not a blocker. |

**Follow-ups (tracked, not done).**

- Larger font for BPM / SpO2 readouts (12×16 bitmap or 2× scaled
  5×7) for at-a-glance readability.
- Plot a tiny 1-line PPG waveform on the bottom half of the OLED
  using rolling IR samples. Reuses the same SPSC ring contents
  the USB consumer already drains.
- Page-mode partial flush (write only the row(s) that changed) to
  drop refresh cost from ~25 ms → ~3 ms. Worth it once we want
  >1 Hz dashboard updates.
