# OxiNode — RP2040-Zero firmware

The first prototype path. Targets the Waveshare RP2040-Zero with a
MAX30102 breakout wired to I²C0 + GP6 INT. Built on pico-sdk 2.2.

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

Sample stream:

```jsonl
{"status":"boot","build":"v0.1.0","sda":4,"scl":5,"int":6,"i2c_hz":100000}
{"status":"ready","reason":"build=v0.1.0"}
{"t":12345,"ir":123456,"red":98765,"hr":72,"spo2":98}
{"t":12365,"ir":123890,"red":98910,"hr":72,"spo2":98}
{"t":13345,"alive":1,"edges":42,"hr":72,"spo2":98}
```

Send `MODE BIN\n` to switch to length-prefixed CRC-16/CCITT frames
for the Python desktop client; `MODE JSON\n` switches back. Wire
format is documented in `/docs/PROTOCOL.md`.

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
