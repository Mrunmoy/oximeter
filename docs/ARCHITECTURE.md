# OxiNode Architecture

## What this document is

The big-picture system design for OxiNode. It explains how a sample travels from
the MAX30102 photodiodes to your laptop screen, how the same C++17 driver core
runs on two unrelated MCUs, and which thread/core/task does which work on each
target. Read this before reading any source file. For the *why* behind every
non-obvious choice, see [`DESIGN.md`](DESIGN.md). For wire-level details of the
host link, see [`PROTOCOL.md`](PROTOCOL.md). For pinouts, see
[`HARDWARE.md`](HARDWARE.md).

---

## End-to-end data flow

A single optical sample passes through five stages on its way to a number on a
screen. Every stage is in exactly one of three buckets: **portable** (lives in
`lib/max3010x/`, host-tested, knows nothing about pico-sdk or ESP-IDF),
**platform** (lives in `firmware/<target>/`, owns I/O), or **host** (lives in
`host/`, runs on x86).

```
┌─ MAX30102 sensor ─────────────────────────────────────────────┐
│  Red + IR LEDs pulse → photodiode → 18-bit ADC → 32-sample    │
│  on-chip FIFO → INT pin pulled low when FIFO_A_FULL hits      │
└──────────────────────┬───────────────────────────────────────┘
                       │  open-drain, active low
                       ▼
┌─ Platform layer (firmware/<target>/) ─────────────────────────┐
│  GPIO ISR ─▶ task notify ─▶ data task wakes                   │
│  data task ▶ I²C burst read of FIFO ▶ Sample{ir, red, t}      │
└──────────────────────┬───────────────────────────────────────┘
                       │  Sample stream, ~50 Hz default
                       ▼
┌─ Portable driver core (lib/max3010x/) ────────────────────────┐
│  Max30102      register R/W, FIFO drain, config, calibration  │
│  HrDetector    DC removal, bandpass, peak detect, BPM smooth  │
│  Spo2Algo      ratio-of-ratios → Maxim polynomial → SpO2 %    │
│  Observer      callback fan-out: (Sample, HrSpo2) → link      │
└──────────────────────┬───────────────────────────────────────┘
                       │  {t, ir, red, hr, spo2} record
                       ▼
┌─ Link layer (firmware/<target>/) ─────────────────────────────┐
│  RP2040:    UsbCdcLink → JSON-Lines (default) | binary frame  │
│  ESP32-S3:  WsLink     → WebSocket text JSON to phone browser │
└──────────────────────┬───────────────────────────────────────┘
                       │  USB-CDC bytes  /  Wi-Fi frames
                       ▼
┌─ Host (host/) ────────────────────────────────────────────────┐
│  picocom        eyeball JSON-Lines                            │
│  desktop_client Python TUI, parses binary frames, draws chart │
│  browser        HTML chart from ESP32 (phase 2)               │
└───────────────────────────────────────────────────────────────┘
```

The contract between layers is narrow on purpose. The driver core never sees
USB or sockets; the link layer never sees registers or DSP state. This is what
makes the same `lib/max3010x/` build cleanly under host gtest, pico-sdk, and
ESP-IDF without `#ifdef`.

---

## The IHal indirection

`lib/max3010x/include/IHal.hpp` is the only thing the driver core uses to touch
the outside world. It is intentionally a 4-method abstract base, not a
template, because the driver does not care about compile-time dispatch — the
HAL is constructed once at boot and the v-table cost is invisible against an
I²C transaction.

| Method | Purpose | Typical impl on RP2040 | Typical impl on ESP32-S3 |
|--------|---------|------------------------|--------------------------|
| `i2cWriteThenRead(addr, wr, wlen, rd, rlen)` | One register transaction | `i2c_write_blocking` + `i2c_read_blocking` | `i2c_master_transmit_receive` |
| `gpioIntAttach(cb, ctx)` | Subscribe to MAX30102 INT falling edge | `gpio_set_irq_enabled_with_callback` | `gpio_isr_handler_add` |
| `millis()` | Monotonic millisecond clock for sample timestamps | `to_ms_since_boot(get_absolute_time())` | `esp_timer_get_time() / 1000` |
| `delayMs(ms)` | Coarse blocking delay for power-on settle | `sleep_ms` | `vTaskDelay(pdMS_TO_TICKS(ms))` |

Host tests provide `FakeI2cHal` from `host/tests/fakes/`, which records writes,
queues read responses, and exposes a `triggerInt()` to simulate the FIFO-full
edge. This is enough to test the entire driver and DSP stack against canned
sample sequences without any hardware.

A platform port is "complete" when it ships an implementation of these four
methods and a few lines in `main.cpp` to wire them up. **No other code in
`lib/` should change to add a new platform.** CI host build enforces this by
refusing to compile if any `lib/max3010x/**/*.cpp` includes a platform header.

---

## Interrupt-driven sample path

Polling the FIFO is wasteful and adds jitter. OxiNode treats every sample as an
event:

1. MAX30102 fills its FIFO. When occupancy crosses the
   `FIFO_A_FULL` watermark (configured to 1 by default for low-latency
   prototype mode, raised later for power), it asserts `INT` low.
2. The host MCU's GPIO peripheral fires an edge interrupt. The ISR is
   intentionally tiny — it does **not** read I²C and **does not allocate**. It
   only signals the data task.
3. The data task wakes, calls `Max30102::drainFifo()`, which issues one I²C
   burst read of `FIFO_DATA` until the read pointer catches up.
4. Each `Sample` is fed through `HrDetector` and `Spo2Algo` (see
   [`lib/max3010x/src/Spo2Algo.cpp`](../lib/max3010x/src/Spo2Algo.cpp) and
   [`lib/max3010x/src/HrDetector.cpp`](../lib/max3010x/src/HrDetector.cpp)).
5. The `Observer` callback fires, the link layer formats and pushes one record
   to the host.

The ISR-to-task hop is the only place where the two platforms diverge
meaningfully — see threading model below.

---

## Threading model — RP2040

The Pico has two cores and a hardware multicore FIFO. We pin work explicitly:

| Core | Role |
|------|------|
| **core0** | USB stack (tinyusb), `UsbCdcLink` reader and writer, JSON serialiser, command parser, supervisory state |
| **core1** | `Max30102` driver, `HrDetector`, `Spo2Algo`, observer fan-out |

Why split this way: USB is non-trivially blocking under tinyusb when the host
isn't draining its IN endpoint, and we never want to delay the next FIFO drain
because someone unplugged picocom. Putting the driver on its own core means
the worst-case host-side stall just back-pressures the link queue, not the
sensor.

The cross-core handoff:

```
GPIO ISR (runs on whichever core enabled the IRQ — we enable on core1)
    │   pushes a 1-word "go" sentinel
    ▼
multicore_fifo (HW, lockless)
    │
    ▼
core1 data task (a tight loop with multicore_fifo_pop_blocking)
    │
    │  drains MAX30102 FIFO, runs DSP
    ▼
SPSC ring → core0 link writer
    (lib/max3010x/include/RingBuffer.hpp, single-producer/single-consumer,
     fixed capacity, no allocations)
```

The SPSC ring is sized to absorb roughly 200 ms of samples at 50 Hz so a brief
USB stall never drops data; if the host is gone for longer than that the
ring's tail sentinel triggers a `{"status":"link_overrun", ...}` event.

## Threading model — ESP32-S3 (phase 2)

ESP-IDF gives us FreeRTOS, so we use it the way it wants to be used:

| Task | Core | Priority | Stack |
|------|------|----------|-------|
| `oxi_data` | pinned to APP_CPU (core1) | configMAX_PRIORITIES − 2 | 4 KB |
| `oxi_link` | pinned to PRO_CPU (core0) | default | 4 KB |
| Wi-Fi/LWIP system tasks | PRO_CPU | high | (system) |
| `httpd` / WebSocket | PRO_CPU | default | 4 KB |

The ISR uses `xTaskNotifyFromISR()` to wake `oxi_data`; `oxi_data` posts
samples through a FreeRTOS queue (capacity 64) to `oxi_link`. The same DSP
code from `lib/` runs unchanged.

Pinning `oxi_data` to APP_CPU keeps the I²C and DSP work off the core that
runs Wi-Fi softirq, which has periodic latency spikes when the radio
re-associates.

---

## Module responsibilities

### Portable (`lib/max3010x/`)

| Module | File(s) | Responsibility |
|--------|---------|----------------|
| `Max30102` | `Max30102.{hpp,cpp}` | Register map, init sequence, FIFO drain, mode/sample-rate/LED-current config, optional die-temperature read |
| `Registers` | `Registers.hpp` | All register addresses and bitfields as `static constexpr`. No code, no state. |
| `Sample` | `Sample.hpp` | POD: `{uint32_t t_ms, uint32_t ir, uint32_t red}` |
| `HrDetector` | `HrDetector.{hpp,cpp}` | DC removal (running mean), bandpass (0.5–4 Hz biquad), peak detection on the IR channel, BPM low-pass smoothing |
| `Spo2Algo` | `Spo2Algo.{hpp,cpp}` | AC/DC ratio per channel, ratio-of-ratios `R = (AC_red/DC_red)/(AC_ir/DC_ir)`, Maxim 4th-order polynomial → SpO2 percentage |
| `Observer` | `Observer.hpp` | Single-callback hook the platform layer registers to receive `(Sample, HrSpo2)` records |
| `IHal` | `IHal.hpp` | The 4-method abstract base described above |
| `RingBuffer` | `RingBuffer.hpp` | SPSC fixed-capacity lock-free ring, `Capacity` is a power-of-2 template parameter, `static_assert`-checked |

### Platform — RP2040 (`firmware/rp2040/`)

| Module | File(s) | Responsibility |
|--------|---------|----------------|
| `PicoI2cHal` | `hal/PicoI2cHal.{hpp,cpp}` | `IHal::i2cWriteThenRead`, `IHal::delayMs`, `IHal::millis` |
| `PicoIntPin` | `hal/PicoIntPin.{hpp,cpp}` | GPIO IRQ wiring, multicore FIFO push from ISR |
| `UsbCdcLink` | `link/UsbCdcLink.{hpp,cpp}` | tinyusb CDC, line-discipline reader, JSON-Lines writer, binary-frame writer, mode switch |
| `JsonFormatter` | `link/JsonFormatter.{hpp,cpp}` | Fixed-buffer JSON line builder, no heap, no `printf` |
| `BinaryFramer` | `link/BinaryFramer.{hpp,cpp}` | CRC-16/CCITT-FALSE, framing per [`PROTOCOL.md`](PROTOCOL.md) |
| `pins.hpp` | `include/board/pins.hpp` | All pin constants — single source of truth |
| `main.cpp` | `main.cpp` | Boot sequence, core1 launch, observer wiring |

### Platform — ESP32-S3 (`firmware/esp32s3/`, phase 2)

| Module | File(s) | Responsibility |
|--------|---------|----------------|
| `Esp32I2cHal` | `hal/Esp32I2cHal.{hpp,cpp}` | New `i2c_master` driver wrapper |
| `Esp32IntPin` | `hal/Esp32IntPin.{hpp,cpp}` | GPIO ISR, `xTaskNotifyFromISR` |
| `WifiManager` | `net/WifiManager.{hpp,cpp}` | STA mode, NVS-stored credentials, AP fallback for first-time setup |
| `WsLink` | `link/WsLink.{hpp,cpp}` | esp_http_server WebSocket endpoint, JSON push, embedded HTML/JS chart |
| `pins.hpp` | `include/board/pins.hpp` | ESP32-S3-Zero pinout |

### Host (`host/`)

| Module | Responsibility |
|--------|----------------|
| `tests/` | Google Test against `lib/max3010x/`, using `FakeI2cHal` and canned waveforms |
| `desktop_client/` | Python TUI; parses both JSON-Lines and binary-frame mode; draws live chart |

---

## Module status

| Module | State | Notes |
|--------|-------|-------|
| `lib/max3010x/Max30102` | Scaffolded | Register map written; init sequence and FIFO drain stubs in place; bring-up against real silicon pending |
| `lib/max3010x/HrDetector` | Scaffolded | Biquad coefficients fixed for 50 Hz sample rate; needs validation against finger-on-sensor data |
| `lib/max3010x/Spo2Algo` | Scaffolded | Maxim coefficients hard-coded; calibration against a reference oximeter is a known TODO |
| `lib/max3010x/RingBuffer` | Scaffolded | Power-of-2 capacity, host-tested for SPSC correctness |
| `host/tests` | Scaffolded | Google Test wired up; `FakeI2cHal` covers register R/W and INT injection |
| `firmware/rp2040/PicoI2cHal` | Scaffolded | Compiles; not exercised against real sensor yet |
| `firmware/rp2040/UsbCdcLink` | Scaffolded | JSON-Lines path complete; binary framer present, mode switch present |
| `firmware/rp2040/main.cpp` | Scaffolded | Boots, runs observer loop with synthetic samples in absence of hardware |
| `firmware/esp32s3/*` | Skeleton only | Builds an empty `app_main`; HAL and Wi-Fi link to be implemented in phase 2 |
| `host/desktop_client` | Scaffolded | Reads JSON-Lines; binary parser stubbed; chart UI minimal |

The "scaffolded" rows above all share the same definition: the file exists, the
build passes on host and target, the public API is shaped right, and the unit
tests for any non-trivial logic are in place — but no end-to-end run against a
soldered MAX30102 has happened yet. First on-board test is gated on the
hardware confirmation in [`HARDWARE.md`](HARDWARE.md).
