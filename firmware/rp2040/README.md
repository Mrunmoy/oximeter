# OxiNode — RP2040-Zero firmware

The first prototype path. Targets the Waveshare RP2040-Zero with a
MAX30102 breakout wired to I²C0 + GP6 INT. Built on pico-sdk 2.2.

## Status

**First light** as of commit `da1dd3c` (2026-04-25). Boots, enumerates
as USB-CDC `2e8a:000a Raspberry Pi Pico` at `/dev/ttyACM0`, streams
JSON-Lines, reads SpO2 = 98 % off a finger placed on the optical
window. See [`/docs/DESIGN.md`](../../docs/DESIGN.md) entries D-11
(USB descriptor collision) and D-12 (hybrid IRQ + polled drain) for
the bring-up gotchas.

## Open issues

1. **`HrDetector` does not lock onto a real pulse.** With `cfg.avg
   = AVG_4` the HR field stuck at 31 BPM (envelope settling-time
   mismatched to physical time at 25 Hz output rate). With
   `cfg.avg = AVG_1` (current setting, 100 Hz output) the HR stays
   at -1 (signal too noisy for the adaptive envelope to cross
   threshold cleanly). DSP fix lives in
   `lib/max3010x/src/HrDetector.cpp` — needs (a) 0.5–4 Hz
   band-pass pre-filter, (b) faster envelope α at startup,
   (c) tighter peak-fraction. SpO2 path is unaffected (uses
   AC RMS / DC mean over 1 s window, no peak detection).
2. **Host control parser disabled.** `MODE BIN\n` / `MODE JSON\n`
   are not parsed in this build — `pico_stdio_usb` owns the USB
   descriptor and the `tud_cdc_n_*` declarations are gated behind
   its private `tusb_config.h`. Forcing `OXINODE_HAVE_TINYUSB=0`
   in `src/UsbCdcLink.cpp` keeps the build healthy at the cost
   of always-JSON output. Re-enable by writing a custom USB
   descriptor and dropping `pico_stdio_usb`. See D-11.

## Pin map

See `include/board/pins.hpp` (single source of truth) and
`/docs/HARDWARE.md` (canonical wiring with photos).

| Signal | RP2040-Zero pad | MAX30102 |
|--------|-----------------|----------|
| 3V3 OUT | 3V3            | VIN      |
| GND     | GND            | GND      |
| SDA     | GP4 (I²C0)     | SDA      |
| SCL     | GP5 (I²C0)     | SCL      |
| INT     | GP6            | INT      |

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

Sample stream (real capture, finger on sensor):

```jsonl
{"t":22708,"ir":111705,"red":109813,"hr":null,"spo2":98}
{"t":22708,"ir":111546,"red":109744,"hr":null,"spo2":98}
{"t":22770,"ir":111377,"red":109653,"hr":null,"spo2":98}
{"t":22770,"alive":1,"edges":747,"hr":-1,"spo2":98,"probe":0,"cfg":0,"int1":0,"int2":0,"drain":6}
```

The diagnostic-rich `alive` frame (added in commit `11a3b15`) carries
`probe` / `cfg` return codes and live `INTR_STATUS_{1,2}` so a single
line tells you whether the I²C path is healthy and whether the chip
is firing interrupts.

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
