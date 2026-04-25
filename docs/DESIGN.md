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
