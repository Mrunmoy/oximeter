# OxiNode — RP2040-Zero firmware

The first prototype path. Targets the Waveshare RP2040-Zero with a
MAX30102 breakout wired to I²C0 + GP6 INT. Built on pico-sdk 2.2.

## Status

**HR + SpO2 + OLED dashboard all working** as of 2026-04-26.
Boots, enumerates as USB-CDC `2e8a:000a Raspberry Pi Pico` at
`/dev/ttyACM0`, streams JSON-Lines, drives an SSD1306 OLED on I²C1,
and locks onto a real cardiac signal (HR = 78 BPM, SpO2 = 97-98 %
off a finger). 60 s burn-in
(`./scripts/burn-in.sh`) passes with all observability counters
clean and a 1:1 IRQ-to-sample ratio (5777 edges = 5777 samples
drained = 5777 consumed over 230 s, no drops). See
[`/docs/DESIGN.md`](../../docs/DESIGN.md):
- D-11 — `pico_stdio_usb` USB descriptor collision (bring-up gotcha)
- D-12 — hybrid IRQ + 50 ms polled FIFO drain *(superseded)*
- D-13 — Maxim `find_peaks` HR algorithm + AN6845 calibrated SpO2
  quadratic + UG6409 chip-config alignment
- D-14 — pure IRQ-driven drain (no polled fallback)
- D-15 — producer/consumer split, observability counters, watchdog
- **D-16 — GPIO IRQ trampoline must run on core0**, not core1 (the
  fix that made D-14's pure-IRQ design actually work on hardware)
- D-17 — median-of-3 output filter on HR (squashes 1-cycle excursions)
- D-18 — SSD1306 OLED dashboard on I²C1 (GP14/GP15)

## Open issues

1. **Host control parser disabled.** `MODE BIN\n` / `MODE JSON\n`
   are not parsed in this build — `pico_stdio_usb` owns the USB
   descriptor and the `tud_cdc_n_*` declarations are gated behind
   its private `tusb_config.h`. Forcing `OXINODE_HAVE_TINYUSB=0`
   in `src/UsbCdcLink.cpp` keeps the build healthy at the cost
   of always-JSON output. Re-enable by writing a custom USB
   descriptor and dropping `pico_stdio_usb`. See D-11.

## Pin map

See `include/board/pins.hpp` (single source of truth) and
`/docs/HARDWARE.md` (canonical wiring with photos).

| Signal | RP2040-Zero pad | Peripheral |
|--------|-----------------|------------|
| 3V3 OUT | 3V3            | MAX30102 VIN, OLED VDD |
| GND     | GND            | both GNDs |
| SDA     | GP4 (I²C0)     | MAX30102 SDA |
| SCL     | GP5 (I²C0)     | MAX30102 SCL |
| INT     | GP6            | MAX30102 INT (open-drain, internal pull-up) |
| OLED SDA | GP14 (I²C1)   | SSD1306 SDA (400 kHz) |
| OLED SCL | GP15 (I²C1)   | SSD1306 SCL (400 kHz) |

## Build

Inside the Nix dev shell (`nix develop` from the repo root, which
exports `PICO_SDK_PATH`):

```bash
./scripts/build.sh rp2040
# or, manually:
cmake -S firmware/rp2040 -B build/rp2040 -G Ninja
cmake --build build/rp2040 --parallel
```

Artifact lands at `build/rp2040/apps/oxinode/oxinode.uf2`. The
`scripts/build.sh` wrapper additionally copies it to
`build/rp2040/oxinode.uf2` for convenience.

## Flash

The RP2040-Zero appears on the host as USB ID `2e8a:0003 RP2 Boot`
once you hold `BOOT` and plug it in.

```bash
# Drag-and-drop:
cp build/rp2040/oxinode.uf2 /run/media/$USER/RPI-RP2/

# Or via picotool:
./scripts/flash.sh rp2040
# expands to: picotool load -x build/rp2040/oxinode.uf2
```

## Monitor

Boots in JSON-Lines mode (newline-delimited JSON, eyeballable in
any terminal):

```bash
picocom -b 115200 /dev/ttyACM0
# or:
./scripts/monitor.sh
```

Sample stream (real capture, finger on sensor, post D-13):

```jsonl
{"t":23593,"ir":237598,"red":205984,"hr":78,"spo2":97}
{"t":23648,"ir":235406,"red":205169,"hr":78,"spo2":97}
{"t":23701,"ir":234622,"red":204885,"hr":78,"spo2":97}
{"t":23756,"ir":234802,"red":204971,"hr":78,"spo2":97}
{"t":23810,"ir":235893,"red":205384,"hr":78,"spo2":97}
{"t":23918,"ir":236652,"red":205688,"hr":78,"spo2":97}
{"t":24530,"alive":1,"edges":479,"hr":78,"spo2":97,"probe":0,"cfg":0,"int1":0,"int2":0,"drain":2}
```

The diagnostic-rich `alive` frame (added in commit `11a3b15`) carries
`probe` / `cfg` return codes and live `INTR_STATUS_{1,2}` so a single
line tells you whether the I²C path is healthy and whether the chip
is firing interrupts. With the post-D-13 chip config (SR=100 Hz,
SMP_AVE=4 → 25 Hz output rate) you should see roughly 25 sample
lines per second between alive frames; `edges` should advance by
~25/s; `drain` is a small positive number per pump cycle.

Mode-switch commands (`MODE BIN\n` / `MODE JSON\n`) are not parsed in
the current firmware — see "Open issues" above. The wire format
itself is fully specified in `/docs/PROTOCOL.md` and implemented in
the Python client; firmware-side support requires a custom USB
descriptor.

## Layout

```
firmware/rp2040/
├── CMakeLists.txt              ← top level — bootstraps pico-sdk
├── cmake/oxinode_app.cmake     ← oxinode_add_app() helper
├── include/board/pins.hpp      ← pin constants (this board only)
└── apps/oxinode/
    ├── CMakeLists.txt
    ├── main.cpp                ← core0/core1 wiring + banner
    ├── inc/PicoI2cHal.hpp      ← IHal impl over hardware_i2c
    ├── src/PicoI2cHal.cpp
    ├── inc/PicoIntPin.hpp      ← GP6 ISR → multicore FIFO
    ├── src/PicoIntPin.cpp
    ├── inc/UsbCdcLink.hpp      ← JSON / BIN link layer
    ├── src/UsbCdcLink.cpp
    └── inc/StatusLog.hpp       ← {"status":"...","reason":"..."}
```

## Threading model

```
core0 :  USB-CDC pump (1 ms repeating timer)
         host control parser (MODE BIN / MODE JSON)
         1 Hz alive frame
core1 :  PicoIntPin::waitForInterrupt()  ← blocks on inter-core FIFO
         sensor.handleInterrupt()        ← drains MAX30102 FIFO over I²C
         HrDetector + Spo2Algo update    ← per sample
         link.writeSample()              ← per sample, JSON or BIN
```

The GP6 GPIO IRQ runs on core1 and posts a wake token into the
inter-core FIFO. ISR work is bounded to "bump a counter, push a
word"; no I²C, no logging.
