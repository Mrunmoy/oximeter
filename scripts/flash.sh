#!/usr/bin/env bash
#
# Flash dispatcher for OxiNode.
#
# Usage:
#   scripts/flash.sh rp2040                  # picotool load build/rp2040/oxinode.uf2
#   scripts/flash.sh esp32s3 /dev/ttyACM0    # idf.py -p ... flash monitor
#
set -euo pipefail

target="${1:-rp2040}"
port="${2:-}"
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo_root"

case "$target" in
    rp2040)
        uf2=build/rp2040/oxinode.uf2
        if [[ ! -f $uf2 ]]; then
            echo "[flash] $uf2 not found — run scripts/build.sh rp2040 first" >&2
            exit 1
        fi
        if ! command -v picotool >/dev/null 2>&1; then
            echo "[flash] picotool not in PATH — drag-drop $uf2 onto the RPI-RP2 mount instead" >&2
            exit 1
        fi
        echo "[flash] picotool load → $uf2"
        sudo picotool load -x "$uf2"
        ;;
    esp32s3|esp32)
        : "${port:?Need port: scripts/flash.sh esp32s3 /dev/ttyACM0}"
        ( cd firmware/esp32s3 && idf.py -p "$port" flash monitor )
        ;;
    *)
        echo "Unknown target: $target  (expected: rp2040 | esp32s3)" >&2
        exit 2
        ;;
esac
