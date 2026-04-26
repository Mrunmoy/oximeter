#!/usr/bin/env bash
# Post-build sanity check: assert the OxiNode `g_ring` SPSC ring is
# placed in the RP2040 SCRATCH_X SRAM bank (`[0x20040000, 0x20041000)`).
#
# Why this exists
#   `firmware/rp2040/apps/oxinode/main.cpp` annotates `g_ring` with
#   `__scratch_x("oxinode")` so the linker maps the symbol into bank 4
#   for contention-free cross-core access (D-15 follow-up). Without
#   an automated check, a future refactor (e.g. someone removing the
#   attribute, or adding a competing variable in the same section
#   that pushes `g_ring` out of range) would silently regress the
#   placement and the perf claim with it.
#
# Usage
#   ./scripts/check-scratch-x.sh <path-to-oxinode.elf>
#
# Returns 0 on success, non-zero on failure. Prints a one-line PASS
# or a multi-line FAIL with the offending address.
#
# Tooling
#   Uses `arm-none-eabi-nm` from the toolchain on PATH (Nix dev shell
#   provides this). Falls back to plain `nm` for host-side smoke tests.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <path-to-oxinode.elf>" >&2
    exit 2
fi

elf="$1"
if [[ ! -f "$elf" ]]; then
    echo "[scratch_x] FAIL: $elf does not exist" >&2
    exit 2
fi

# Pick whichever nm is available — arm-none-eabi-nm is the right one
# for the cross-compiled .elf, but plain nm reads ELF too and is
# enough if the toolchain isn't on PATH.
if command -v arm-none-eabi-nm >/dev/null 2>&1; then
    nm_cmd=arm-none-eabi-nm
else
    nm_cmd=nm
fi

# `g_ring` is in an anonymous namespace so its mangled name carries
# the canonical `_ZN12_GLOBAL__N_1` prefix. GCC also stamps the
# anonymous-namespace globals with an `L` (local linkage) marker, and
# the identifier-length prefix that follows (`6g_ring`, `11g_ringDrops`,
# `9g_ringHwm`) lets us pick the bare `g_ring` symbol without matching
# its sibling counters. Anchor on `6g_ringE` (length-6 identifier
# followed by namespace-end `E`).
addr=$("$nm_cmd" --print-size "$elf" \
       | awk '/6g_ringE$/ {print $1}' \
       | head -1)

if [[ -z "$addr" ]]; then
    echo "[scratch_x] FAIL: could not find g_ring symbol in $elf" >&2
    echo "[scratch_x]       (was the __scratch_x attribute removed?)" >&2
    exit 1
fi

addr_dec=$((16#$addr))
range_start=$((16#20040000))
range_end=$((16#20041000))

if (( addr_dec < range_start || addr_dec >= range_end )); then
    echo "[scratch_x] FAIL: g_ring at 0x$addr is not in scratch_x" >&2
    echo "[scratch_x]       expected range [0x20040000, 0x20041000)" >&2
    echo "[scratch_x]       This means the __scratch_x section was" >&2
    echo "[scratch_x]       removed, displaced by another variable," >&2
    echo "[scratch_x]       or the linker script changed." >&2
    exit 1
fi

echo "[scratch_x] g_ring at 0x$addr (in scratch_x bank 4) ✓"
