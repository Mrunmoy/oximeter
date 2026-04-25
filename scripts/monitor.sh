#!/usr/bin/env bash
#
# Open a serial monitor on the first OxiNode device.
#
# Usage:
#   scripts/monitor.sh                       # auto-pick /dev/ttyACM*
#   scripts/monitor.sh /dev/ttyACM0          # explicit port
#   scripts/monitor.sh --client              # launch host/desktop_client TUI
#
set -euo pipefail

if [[ "${1:-}" == "--client" ]]; then
    exec python3 -m oxinode_client "${@:2}"
fi

port="${1:-}"
if [[ -z $port ]]; then
    candidates=(/dev/ttyACM* /dev/serial/by-id/usb-Espressif*)
    for c in "${candidates[@]}"; do
        if [[ -e $c ]]; then
            port=$(readlink -f "$c")
            break
        fi
    done
fi

if [[ -z $port || ! -e $port ]]; then
    echo "[monitor] no serial device found — plug in the board or pass an explicit port" >&2
    exit 1
fi

echo "[monitor] picocom $port @ 115200  (Ctrl-A then Ctrl-X to exit)"
exec picocom -b 115200 --imap lfcrlf "$port"
