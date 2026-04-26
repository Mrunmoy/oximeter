#!/usr/bin/env bash
# OxiNode RP2040 burn-in test.
#
# Captures alive frames from a flashed-and-running device for a fixed
# duration and asserts the firmware-level observability counters
# stayed within budget. Intended as a PR-merge gate (CI/manual): a PR
# that introduces ISR overruns, ring drops, DSP-budget regressions,
# or unhandled brownouts will trip one of the assertions below.
#
# Usage (inside the Nix dev shell — needs `jq`):
#
#   ./scripts/burn-in.sh                  # default: /dev/ttyACM0, 60 s
#   DEVICE=/dev/ttyACM1 ./scripts/burn-in.sh
#   DURATION=120 ./scripts/burn-in.sh
#
# Pre-condition: the firmware must already be flashed and the device
# enumerated as a USB-CDC device. The script does not flash. Use
# `./scripts/flash.sh rp2040` first if needed.
#
# The thresholds below are *witnesses* of the firmware's own
# definitions — `kDspBudgetUs` etc. live in main.cpp. The only
# semantically-meaningful number defined here is the count of
# permitted brownouts (`pwr_rdy <= 1` allows for one PWR_RDY at the
# moment the host plugged the device in), which is hardware-bench
# tolerance, not a firmware constant.

set -euo pipefail

DEVICE="${DEVICE:-/dev/ttyACM0}"
DURATION="${DURATION:-60}"
LOGFILE="$(mktemp -t oxinode-burnin.XXXXXX.jsonl)"
trap 'rm -f "$LOGFILE"' EXIT

if [ ! -e "$DEVICE" ]; then
    echo "[burn-in] FAIL: $DEVICE not found." >&2
    echo "[burn-in]       flash and let the board enumerate first," >&2
    echo "[burn-in]       e.g.: ./scripts/flash.sh rp2040" >&2
    exit 2
fi
if ! command -v jq >/dev/null 2>&1; then
    echo "[burn-in] FAIL: jq not in PATH (enter the Nix dev shell)." >&2
    exit 2
fi

echo "[burn-in] capturing $DURATION s from $DEVICE → $LOGFILE"
stty -F "$DEVICE" 115200 raw -echo -echoe -echok -echoctl -echoke 2>/dev/null || true
timeout "$DURATION" cat "$DEVICE" > "$LOGFILE" || true

ALIVE_LINES=$(grep -c '"alive":1' "$LOGFILE" || true)
echo "[burn-in] captured $ALIVE_LINES alive frames"

# Expect ~1 alive/s; allow 1/3 budget for boot delay + USB warmup.
MIN_ALIVE=$(( DURATION / 3 ))
if [ "$ALIVE_LINES" -lt "$MIN_ALIVE" ]; then
    echo "[burn-in] FAIL: only $ALIVE_LINES alive frames; expected ≥ $MIN_ALIVE" >&2
    echo "[burn-in]       the device may have hung or rebooted." >&2
    exit 3
fi

# Counters are monotonic — endpoint of run is sufficient.
LAST=$(grep '"alive":1' "$LOGFILE" | tail -1)
echo "[burn-in] last alive frame:"
echo "$LAST" | jq .

check_max() {
    local field="$1"; local max="$2"; local actual
    actual=$(echo "$LAST" | jq -r ".$field // 0")
    if [ "$actual" -gt "$max" ]; then
        echo "[burn-in] FAIL: $field=$actual exceeds budget ($max)" >&2
        exit 4
    fi
    printf '[burn-in]   %-18s = %-6s (max %s)  ✓\n' "$field" "$actual" "$max"
}

# All counters expected to stay flat over a healthy 60 s run on the
# bench. If any of these trip, a real regression has been introduced.
check_max chip_ovf        0   # chip FIFO overflowed → drain too slow
check_max ring_drops      0   # SW ring full        → consumer too slow
check_max dsp_overbudget  0   # consumer body > kDspBudgetUs
check_max i2c_err         0   # I²C transactions failing
check_max alc_ovf         0   # ambient-light cancellation saturating
check_max pwr_rdy         1   # one allowed (USB-plug brownout)
check_max fault_flags     0   # nothing latched

echo "[burn-in] PASS"
