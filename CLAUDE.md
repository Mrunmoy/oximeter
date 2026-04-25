# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

OxiNode — MAX30102 pulse oximeter showcase. Two firmware targets share one portable driver core:

- **`firmware/rp2040/`** — Waveshare RP2040-Zero, pico-sdk 2.2, USB-CDC link to a desktop client. **First prototype.** Add new features here first.
- **`firmware/esp32s3/`** — Waveshare ESP32-S3-Zero, ESP-IDF v5.5, Wi-Fi + embedded HTML web UI. **Phase 2** — skeleton only at present.
- **`lib/max3010x/`** — portable C++17 driver, HR (peak detection), SpO2 (Maxim ratio-of-ratios). Fully host-testable behind an `IHal` interface. Source of truth for DSP and protocol behaviour.
- **`host/tests/`** — Google Test against a `FakeI2cHal`. New driver / DSP work is test-first here.
- **`host/desktop_client/`** — Python TUI for the RP2040 binary wire mode.

## Hardware status

**RP2040-Zero is wired and working** as of commit `da1dd3c` (first light: SpO2 = 98 % off finger). Canonical wiring (matches `firmware/rp2040/include/board/pins.hpp` and `docs/HARDWARE.md`):

| MAX30102 | RP2040-Zero pin | Notes |
|----------|-----------------|-------|
| VIN | 3V3 (OUT) | breakout LDO accepts 3.3–5 V; user's 3V3 jumper is shorted |
| GND | GND | |
| SDA | GP4 | I²C0, breakout has 4.7 kΩ pull-up |
| SCL | GP5 | I²C0 |
| INT | GP6 | open-drain, active low — internal pull-up enabled in firmware |
| RD | GP7 | *extra pad on user's breakout, harmless — RED LED drive monitor, not driven by firmware* |
| IRQ | GP8 | *extra pad on user's breakout, harmless — duplicate of INT on some breakout revisions, not used by firmware* |

Connected USB devices on this workstation (typical):

- **ESP32-S3-Zero** — `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*` → `/dev/ttyACM0`
- **RP2040-Zero** — appears as `2e8a:0003 RP2 Boot` in BOOTSEL mode (drag-drop UF2), or as `/dev/ttyACM*` once running OxiNode firmware (USB-CDC).

## Build commands

```bash
# Inside the Nix dev shell
nix develop

./scripts/build.sh rp2040               # → build/rp2040/oxinode.uf2
./scripts/build.sh esp32s3              # → build/esp32s3/oxinode.bin
./scripts/build.sh host-tests           # → build/host/oxinode_tests, then runs them

# Or via Docker (no Nix needed)
./docker/docker-build.sh rp2040
./docker/docker-build.sh esp32s3

# Flash + monitor
./scripts/flash.sh rp2040               # picotool load
./scripts/flash.sh esp32s3 /dev/ttyACM0 # idf.py -p ... flash monitor
./scripts/monitor.sh                    # picocom /dev/ttyACM* @ 115200
```

Single-test run (gtest filter):
```bash
ctest --test-dir build/host -R Max30102 --output-on-failure
# or
build/host/oxinode_tests --gtest_filter='Max30102Test.*'
```

## Code style — non-obvious points

(For the obvious style — Allman, 4-space, `m_`, `kCamelCase`, no exceptions/RTTI — see `~/.claude/CLAUDE.md`.)

- **Driver-core files in `lib/max3010x/` MUST NOT include any platform header** (`pico/...`, `esp_*`, `freertos/...`). The `IHal` indirection exists precisely to keep this contract; CI host build will catch violations.
- **All driver / DSP changes are written test-first** in `host/tests/`. The platform paths only adapt — they don't add behaviour.
- **Wire protocol (`docs/PROTOCOL.md`) is versioned** — `binary frame TYPE` enum is append-only; never repurpose a value. Bump the schema version field if you change a payload.
- **Pin allocation lives in `firmware/<target>/include/board/pins.hpp`**, never inline in main.cpp. Adding a new board variant means a new `pins.hpp`, not `#ifdef BOARD_*` scattered across the firmware.
- **The MAX30102 INT line is open-drain.** Firmware must enable an internal pull-up on the GPIO; without one, you'll see spurious edges on every wire jiggle.

## Workflow

1. Decide whether the change is portable (driver / DSP / framer) or platform-specific (HAL / link).
2. If portable: write the gtest first under `host/tests/`, get it failing, then implement in `lib/max3010x/`.
3. If platform-specific: scope to `firmware/<target>/` only; do not let platform headers bleed back into `lib/`.
4. `./scripts/build.sh host-tests` first, then `./scripts/build.sh rp2040` (and esp32s3 if touched).
5. Update `docs/DESIGN.md` if the change is a non-obvious decision.

## Datasheets

Committed under `datasheets/`. Source URLs and provenance are in `docs/DATASHEETS.md`. To re-fetch, `./datasheets/fetch.sh`.
