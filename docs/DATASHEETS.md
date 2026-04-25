# OxiNode Datasheets

## What this document is

A manifest of every vendor PDF this project depends on, where to get it, and
which OxiNode component reads from it. Datasheets that are committed to the
repo live under `datasheets/`; refresh them with `./datasheets/fetch.sh`.
Datasheets that are too volatile to commit (e.g. the Waveshare wiki silkscreen
images, which are revised in place without a versioned URL) are referenced by
URL only and are not part of the repo.

If you're reviewing a piece of OxiNode code and want to know "where did this
register address come from?" or "why is the LDO budget 300 mA?", the answer
is in one of these files.

---

## Committed PDFs (under `datasheets/`)

| Filename | Vendor | Version / date on file | Source URL | What it's for in OxiNode |
|----------|--------|------------------------|------------|---------------------------|
| `MAX30102.pdf` | Analog Devices (ex-Maxim) | Datasheet, Rev. 1, October 2018 | <https://www.analog.com/media/en/technical-documentation/data-sheets/MAX30102.pdf> | The sensor. Register map, FIFO behaviour, LED-current and pulse-width tables, INT pin electrical characteristics, I²C timing. Read by `lib/max3010x/Max30102.cpp` and `Registers.hpp`. |
| `RP2040_datasheet.pdf` | Raspberry Pi Ltd. | Datasheet (latest at fetch time) | <https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf> | The MCU on the RP2040-Zero. I²C0 pin multiplexing tables, GPIO IRQ semantics, multicore FIFO description. Read by `firmware/rp2040/hal/PicoI2cHal.cpp` and `PicoIntPin.cpp`. |
| `pico-datasheet.pdf` | Raspberry Pi Ltd. | Datasheet (Raspberry Pi Pico — *not* the RP2040-Zero) | <https://datasheets.raspberrypi.com/pico/pico-datasheet.pdf> | Reference for the original Pico board's electrical and mechanical layout. The Waveshare RP2040-Zero is a different board with a different pinout — this PDF is included as a cross-reference only and is **not** the canonical source for OxiNode pinouts. For RP2040-Zero pin labels see the Waveshare wiki link below. |
| `ESP32-S3_datasheet.pdf` | Espressif Systems | Datasheet v1.x (latest at fetch time) | <https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf> | The MCU on the ESP32-S3-Zero. GPIO matrix, strapping pin behaviour, USB-Serial-JTAG pinout, RAM layout. Read by `firmware/esp32s3/hal/Esp32I2cHal.cpp` (phase 2). |

`./datasheets/fetch.sh` is idempotent: it skips files that are already
present, only downloading missing ones. Re-running after deleting a file
will re-fetch it from the URL above.

---

## Status of `MAX30102.pdf` in the current checkout

`MAX30102.pdf` is present (917 KB, Rev. 1, October 2018) and the driver in
`lib/max3010x/` has been cross-checked against pages 10–15 of it byte-for-byte:

| Item | Datasheet location | Driver source |
|------|--------------------|---------------|
| Register addresses (0x00–0xFF, PART_ID = 0x15) | p.10–11 | `Registers.hpp` |
| INTR_STATUS_1/2 bit positions (A_FULL=B7, PPG_RDY=B6, ALC_OVF=B5, PWR_RDY=B0, DIE_TEMP_RDY=B1) | p.12 | `INT_*` constants in `Registers.hpp` |
| FIFO_CONFIG layout (SMP_AVE[7:5] \| ROLLOVER[4] \| A_FULL[3:0]) | p.10 | `Max30102::configure` |
| MODE_CONFIG layout (SHDN[7], RESET[6], MODE[2:0] = 010/011/111) | p.10–11 | `MODE_*` constants |
| SPO2_CONFIG layout (ADC_RGE[6:5] \| SR[4:2] \| LED_PW[1:0]) | p.10 | `Max30102::configure` |
| FIFO entry order in SpO2 mode (RED 3 B then IR 3 B per sample) | p.15 Fig.2 | `decodeSpo2Entry` |
| 18-bit ADC left-justification, MSB always at bit 17, mask 0x3FFFF | p.14 Tbl.1 | `decodeSpo2Entry` / `decodeHrEntry` |
| FIFO depth = 32, 6 B per entry (SpO2), 192 B max burst | p.14 | `kFifoDepth`, `kBytesPerEntrySpo2` |
| Reading INTR_STATUS_1 clears the latched interrupt | p.12 | `handleInterrupt` |

If you bump the driver against a newer datasheet revision, regenerate this
table or just re-run the host gtest suite — the `FakeI2cHal` exercises the
full register write/burst-read path and will flag any drift.

---

## Referenced but not committed

These are required reading for some OxiNode work but are not appropriate
for the repo — usually because they're in-place HTML pages with no versioned
download, or because the vendor's terms restrict redistribution.

| What | URL | When to read it |
|------|-----|-----------------|
| Waveshare RP2040-Zero wiki page | <https://www.waveshare.com/wiki/RP2040-Zero> | Canonical source for the RP2040-Zero pin labels and silkscreen orientation. The Waveshare page is the authority; the `pico-datasheet.pdf` in this repo is a different board. |
| Waveshare ESP32-S3-Zero wiki page | <https://www.waveshare.com/wiki/ESP32-S3-Zero> | Canonical source for the ESP32-S3-Zero pin labels, WS2812 LED GPIO, USB-JTAG configuration. |
| Maxim "Recommended Configurations and Operating Profiles for MAX30101/MAX30102 EV Kits" — App Note 6409 | <https://www.analog.com/en/technical-articles/recommended-configurations-and-operating-profiles-for-max30101-max30102-ev-kits.html> | Reference for the LED-current, sample-rate, and pulse-width combinations that produce a usable PPG. Source of the default register values in `lib/max3010x/Max30102.cpp`. |
| Maxim "Guidelines for SpO2 Measurement Using the MAX32664 Sensor Hub" / Maxim PPG application notes | <https://www.analog.com/en/applications/technology/health-fitness-pavilion-home/optical-monitors-spo2-pulse-monitors.html> | Background on the ratio-of-ratios method and the polynomial form used in `lib/max3010x/src/Spo2Algo.cpp`. |
| pico-sdk documentation | <https://raspberrypi.github.io/pico-sdk-doxygen/> | API reference for `i2c_*`, `gpio_*`, `multicore_*` calls used by `firmware/rp2040/hal/`. |
| ESP-IDF v5.5 reference | <https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/> | API reference for `i2c_master`, `gpio`, `esp_http_server` used by `firmware/esp32s3/` (phase 2). |

When a Waveshare wiki revision changes a pin label, fix
`firmware/<target>/include/board/pins.hpp` and the wiring tables in
[`HARDWARE.md`](HARDWARE.md) in the same commit. Do not rely on a
months-old screenshot.

---

## Refresh procedure

```bash
cd /path/to/oxinode
./datasheets/fetch.sh
git status datasheets/
# review newly downloaded PDFs (size sanity check, openable, vendor name visible)
git add datasheets/*.pdf
git commit -m "datasheets: refresh from upstream"
```

The fetcher script forces HTTP/1.1 against the Analog Devices CDN because
their HTTP/2 endpoint occasionally rejects scripted clients with a 403.
If a future fetch fails for a different reason (vendor URL changed), update
the URL in `datasheets/fetch.sh` and in the corresponding row of the table
above in the same commit.
