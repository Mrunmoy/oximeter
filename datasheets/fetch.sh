#!/usr/bin/env bash
#
# Fetch (or refresh) the vendor datasheets that this project depends on.
# Re-runs are idempotent — files are only re-downloaded if missing.
#
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

declare -A SOURCES=(
    [MAX30102.pdf]="https://www.analog.com/media/en/technical-documentation/data-sheets/MAX30102.pdf"
    [ESP32-S3_datasheet.pdf]="https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf"
    [RP2040_datasheet.pdf]="https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf"
    [pico-datasheet.pdf]="https://datasheets.raspberrypi.com/pico/pico-datasheet.pdf"
)

for name in "${!SOURCES[@]}"; do
    if [[ -f $name ]]; then
        echo "[datasheets] keep   $name"
        continue
    fi
    url="${SOURCES[$name]}"
    echo "[datasheets] fetch  $name  ←  $url"
    # ADI servers are picky about HTTP/2; force HTTP/1.1.
    curl -fL --http1.1 --retry 3 -A "Mozilla/5.0" -o "$name" "$url"
done

echo "[datasheets] all present:"
ls -lh ./*.pdf
