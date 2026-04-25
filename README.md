# OxiNode

[![Status](https://img.shields.io/badge/Status-Scaffolded-yellow)]()
[![License](https://img.shields.io/badge/License-MIT-blue)]()
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)]()
[![Targets](https://img.shields.io/badge/Targets-RP2040%20%7C%20ESP32--S3-success)]()

> **Heart rate and SpO₂ from a MAX30102, on either a RP2040-Zero or an ESP32-S3-Zero, off one portable C++17 driver core.**

OxiNode is a deliberately small, professional-grade firmware showcase. The same `lib/max3010x/` driver runs on two boards behind a 4-method HAL interface. Pick your platform; the MCU-specific code is < 300 LoC.

| Path | Board | Link to host | Use case |
|------|-------|--------------|----------|
| **RP2040** *(prototype, default)* | Waveshare RP2040-Zero | USB-CDC, JSON-Lines or binary CRC frames | Plug into a laptop, watch numbers in `picocom` or the Python TUI |
| **ESP32-S3** *(phase 2)* | Waveshare ESP32-S3-Zero | Wi-Fi STA + WebSocket, embedded HTML | Open the IP in your phone browser, watch a live chart |

Both paths are interrupt-driven: the MAX30102 drives an active-low INT line, an ISR posts to a task, the task drains the FIFO over I²C. No polling.

---

## Quick start

You only need **one** of the two dev environments. Both reproduce the toolchain bit-for-bit; pick whichever you already have.

### Option A — Nix flake *(recommended)*

```bash
git clone <this-repo> oxinode && cd oxinode
nix develop                             # drops you in a shell with both toolchains
./scripts/build.sh rp2040               # → build/rp2040/oxinode.uf2
./scripts/build.sh esp32s3              # → build/esp32s3/oxinode.bin (phase 2)
```

The flake pins `pico-sdk` 2.2.0 (with submodules — required for tinyusb) and pulls `gcc-arm-embedded`, `picotool`, `openocd-rp2040`, `picocom`, plus `cmake`/`ninja`/`python3`. ESP-IDF v5.5 is fetched on first use of the ESP32 shell because Espressif's installer stages ~400 MB of toolchain that doesn't belong in the Nix store.

### Option B — Docker

```bash
./docker/docker-build.sh rp2040         # builds firmware/rp2040 inside a pico-sdk image
./docker/docker-build.sh esp32s3        # builds firmware/esp32s3 inside espressif/idf:release-v5.5
```

The Docker images are immutable — your host stays clean, but you'll need `picotool` / `idf.py flash` on the host to actually flash, since USB pass-through to a container is fiddly.

### Flashing

**RP2040-Zero** — hold `BOOT`, plug USB, release. Drag-drop the `.uf2`, *or*:
```bash
./scripts/flash.sh rp2040               # picotool load + reboot to app
./scripts/monitor.sh                    # picocom /dev/ttyACM* @ 115200
```

**ESP32-S3-Zero** — over the on-chip USB-JTAG (no DTR/RTS dance required):
```bash
./scripts/flash.sh esp32s3 /dev/ttyACM0 # idf.py -p ... flash monitor
```

---

## Hardware

**STOP — do not solder yet.** First confirm the silkscreen on your MAX30102 breakout matches one of the variants in [`docs/HARDWARE.md`](docs/HARDWARE.md). Once confirmed, the wiring is:

```
RP2040-Zero            MAX30102 breakout (GY-style 7-pin)
─────────────          ─────────────────────────────────────
  3V3 (OUT)  ────────▶ VIN              (sensor accepts 3.3–5 V)
  GND        ────────▶ GND
  GP4 / SDA  ◀──────▶ SDA               (I²C0, 4.7 kΩ pull-up on breakout)
  GP5 / SCL  ────────▶ SCL              (I²C0, 4.7 kΩ pull-up on breakout)
  GP6        ◀────── INT                (open-drain, active low; internal pull-up enabled)
  ──         ──        IRD              (leave floating — LED-drive monitor pin)
  ──         ──        RD               (leave floating — LED-drive monitor pin)
```

Full pin tables for both boards plus a fallback wiring for ESP32-S3 are in [`docs/HARDWARE.md`](docs/HARDWARE.md).

---

## Architecture at a glance

```
                ┌────────────────────────────────────────┐
                │       lib/max3010x  (portable C++17)   │
                │                                        │
                │   Max30102 ──▶ HrDetector ──▶ Sample   │
                │      │                          │      │
                │      ▼                          ▼      │
                │   Spo2Algo ────────────────▶ Observer  │
                └─────▲──────────────────────────────┬───┘
                      │ IHal (i2c, gpio-int, time)   │
   ┌──────────────────┴──────────┐    ┌──────────────┴────────────┐
   │ firmware/rp2040  PicoI2cHal │    │ firmware/esp32s3 (phase 2)│
   │  PicoIntPin → multicore FIFO│    │  Esp32I2cHal              │
   │  UsbCdcLink  → JSON / binary│    │  WifiManager + WS server  │
   └─────────────────────────────┘    └───────────────────────────┘
                      │                            │
                      ▼                            ▼
                 USB-CDC                    Wi-Fi → phone
              picocom / Python TUI          browser chart
```

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the long form, and [`docs/DESIGN.md`](docs/DESIGN.md) for the *why* behind every nontrivial choice.

---

## Wire protocol (RP2040 USB-CDC)

Boots in **JSON-Lines** mode — eyeballable in any serial terminal:

```jsonl
{"t":12345,"ir":123456,"red":98765,"hr":72,"spo2":98}
{"t":12365,"ir":123890,"red":98910,"hr":72,"spo2":98}
```

Send `MODE BIN\n` to switch to length-prefixed CRC-16/CCITT frames for the Python desktop client. Full spec, frame layout, and CRC reference in [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

---

## Repository layout

```
oxinode/
├── datasheets/             ← Vendor PDFs (committed; fetcher in datasheets/fetch.sh)
├── docs/                   ← ARCHITECTURE / DESIGN / HARDWARE / PROTOCOL
├── docker/                 ← Dockerfile.rp2040, Dockerfile.esp32, wrapper script
├── flake.nix               ← Nix dev shell (rp2040 + esp32 in one shell)
├── lib/max3010x/           ← Portable driver core (host-tested)
├── firmware/rp2040/        ← Pico-SDK firmware (priority)
├── firmware/esp32s3/       ← ESP-IDF firmware (phase 2)
├── host/tests/             ← Google Test, FakeI2cHal — runs on x86
├── host/desktop_client/    ← Python TUI for binary mode
└── scripts/                ← build.sh, flash.sh, monitor.sh
```

---

## Status

| Component | State |
|-----------|-------|
| Portable driver core (`lib/max3010x/`) | Scaffolded — driver, registers, HR/SpO2 algos in place |
| Host gtest suite | Scaffolded |
| RP2040 firmware | Scaffolded — compiles before sensor is wired; ready for bring-up |
| Python desktop client | Scaffolded — reads JSON-Lines and binary frames |
| ESP32-S3 firmware | **Skeleton only — phase 2** |
| First on-board test | Pending soldering (see `docs/HARDWARE.md`) |

---

## License

MIT. See [`LICENSE`](LICENSE).
