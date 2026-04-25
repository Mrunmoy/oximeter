# OxiNode — ESP32-S3-Zero firmware (phase 2)

> **Phase 2 skeleton.** The first OxiNode prototype runs on the
> Waveshare RP2040-Zero (`firmware/rp2040/`). This directory holds the
> ESP-IDF v5.5 firmware that brings up the same `lib/max3010x/` driver
> behind Wi-Fi + an embedded HTML viewer, so the user can open the
> device IP on a phone and see live HR / SpO₂.

## Hardware

Waveshare ESP32-S3-Zero (1× WS2812 status LED on GPIO21).

| MAX30102 breakout | ESP32-S3 GPIO | Notes |
|---|---|---|
| VIN | 3V3 | breakout LDO accepts 3.3–5 V |
| GND | GND | |
| SDA | **GPIO8** | I²C0 SDA |
| SCL | **GPIO9** | I²C0 SCL |
| INT | **GPIO10** | open-drain, active-low; internal pull-up enabled |

## Build

### Docker (no host toolchain needed)

```bash
# from the repo root
./docker/docker-build.sh esp32s3
```

The image is `espressif/idf:release-v5.5`; first run pulls the toolchain.

### Native (Linux + IDF v5.5 submodule)

```bash
cd firmware/esp32s3
git submodule update --init --recursive third_party/esp-idf
( cd third_party/esp-idf && ./install.sh esp32s3 )
source env.sh
idf.py set-target esp32s3
idf.py build
```

### Configure Wi-Fi

```bash
source env.sh
idf.py menuconfig
#  → "OxiNode Configuration"
#       Wi-Fi STA SSID
#       Wi-Fi STA password
#       Soft-AP fallback SSID prefix     (default: OxiNode)
#       Soft-AP fallback password        (default: oxinode2026)
#       HTTP server port                 (default: 80)
#       WebSocket server port            (default: 81)
```

If the configured STA network is unavailable for 30 seconds the device
falls back to soft-AP mode with SSID `OxiNode-XXXX` (last 4 hex digits
of the MAC). Default soft-AP IP is `192.168.4.1`.

## Flash & monitor

The S3-Zero exposes only the on-chip USB-Serial-JTAG; it appears as
`/dev/ttyACM0`. No DTR/RTS dance.

```bash
idf.py -p /dev/ttyACM0 flash monitor
# Ctrl-] to exit the monitor.
```

Then open the device IP — `http://<sta-ip>/` (or `http://192.168.4.1/`
if you joined the soft-AP) on your phone.

## Phone view

```
┌─────────────────────────┐
│  OxiNode  [connected]   │
│           [finger ok]   │
│                         │
│   ❤  72 bpm             │
│   🔵 98 %               │
│                         │
│  IR — last 60 s         │
│  ╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲       │
│                         │
│  ws://…/ws              │
└─────────────────────────┘
```

*(Screenshot placeholder — drop a real PNG here once the sensor is on
the bench.)*

## What's in this skeleton

| File | Role |
|---|---|
| `main/main.cpp` | Glue: STA join → HTTP+WS → I²C HAL → driver → INT pin → sensor task on core 1 |
| `main/inc/Esp32I2cHal.{hpp,cpp}` | Implements `oxinode::max3010x::IHal` against IDF v5.5 `i2c_master_*` API |
| `main/inc/Esp32IntPin.{hpp,cpp}` | FALLING-edge ISR + binary semaphore handoff to sensor task |
| `main/inc/WifiManager.{hpp,cpp}` | STA join with soft-AP fallback after 30 s |
| `main/inc/WebSocketServer.{hpp,cpp}` | Embedded HTML at `/`, WS at `/ws`, fixed 4-client array |
| `main/inc/Heartbeat.{hpp,cpp}` | 1 Hz status indicator on the on-board WS2812 (GPIO21) |
| `main/index.html` | Self-contained viewer — no CDN, no framework, ~200 lines |
| `main/Kconfig.projbuild` | Wi-Fi credentials + ports under "OxiNode Configuration" |
| `partitions.csv` | 4 MB flash, factory + 2× OTA |
| `sdkconfig.defaults` | esp32s3 target, USB-CDC console, 160 MHz CPU, 1 kHz tick |

## JSON wire format

Identical schema to the RP2040 JSON-Lines mode (`docs/PROTOCOL.md`):

```json
{"t":12345,"ir":123456,"red":98765,"hr":72,"spo2":98}
```

One JSON message per sample, broadcast to every connected WS client.

## Known gaps (skeleton only)

- The driver core (`lib/max3010x/`) is being authored in parallel; until
  its sources land the link step will fail with unresolved symbols
  (`Max30102::begin`, `Max30102::handleInterrupt`, etc.). The
  `firmware/esp32s3/` half is independent of that work and compiles
  against the IHal contract alone.
- The on-board WS2812 driver uses the IDF managed component
  `espressif/led_strip`. If that component isn't pulled in, the
  Heartbeat falls back to a once-per-second `ESP_LOGI("alive")`.
- TLS / mDNS / OTA endpoints are not wired yet — phase 3 work.
