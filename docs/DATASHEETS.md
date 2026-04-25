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

At the time this document was generated, automated fetch attempts to the
Analog Devices CDN failed from this environment (network restriction).
**`MAX30102.pdf` is therefore not present in `datasheets/` in this commit.**
Run `./datasheets/fetch.sh` from a host with normal outbound HTTPS to
populate it before doing any non-trivial driver work, and check the result
into a follow-up commit. The Adafruit mirror referenced in earlier
revisions of this project (`https://cdn-shop.adafruit.com/product-files/3220/MAX30102.pdf`)
was returning HTTP 404 at the same time and has been removed from the
fallback list.

If neither source is reachable from your build host, the canonical online
copy hosted by Analog Devices (linked in the table above) remains the
authority, and any bring-up work that touches the register map should
cross-check against the live page rather than a stale local PDF.

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
