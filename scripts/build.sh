#!/usr/bin/env bash
#
# Top-level build dispatcher for OxiNode.
#
# Usage:
#   scripts/build.sh rp2040          # → build/rp2040/oxinode.uf2
#   scripts/build.sh host-tests      # → build/host/oxinode_tests, runs ctest
#   scripts/build.sh esp32s3         # → firmware/esp32s3/build/oxinode.bin
#   scripts/build.sh all             # rp2040 + host-tests (esp32s3 if env present)
#
set -euo pipefail

target="${1:-rp2040}"
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo_root"

build_rp2040() {
    : "${PICO_SDK_PATH:?PICO_SDK_PATH not set — run inside 'nix develop' or set it manually}"
    echo "[build] rp2040  (PICO_SDK_PATH=$PICO_SDK_PATH)"
    cmake -S firmware/rp2040 -B build/rp2040 -G Ninja
    cmake --build build/rp2040 --parallel
    if [[ -f build/rp2040/apps/oxinode/oxinode.uf2 ]]; then
        cp build/rp2040/apps/oxinode/oxinode.uf2 build/rp2040/oxinode.uf2
        echo "[build] artifact → build/rp2040/oxinode.uf2"
    fi
}

build_host_tests() {
    echo "[build] host-tests"
    cmake -S host/tests -B build/host -G Ninja
    cmake --build build/host --parallel
    ctest --test-dir build/host --output-on-failure
}

build_esp32s3() {
    echo "[build] esp32s3"
    if ! command -v idf.py >/dev/null 2>&1; then
        echo "[build] idf.py not in PATH — sourcing firmware/esp32s3/env.sh if present"
        if [[ -f firmware/esp32s3/env.sh ]]; then
            # shellcheck source=/dev/null
            source firmware/esp32s3/env.sh
        else
            echo "[build] firmware/esp32s3/env.sh not present — try: ./docker/docker-build.sh esp32s3" >&2
            exit 1
        fi
    fi
    ( cd firmware/esp32s3 && idf.py set-target esp32s3 && idf.py build )
}

case "$target" in
    rp2040)      build_rp2040 ;;
    host-tests)  build_host_tests ;;
    esp32s3|esp32) build_esp32s3 ;;
    all)
        build_host_tests
        build_rp2040
        if command -v idf.py >/dev/null 2>&1 || [[ -f firmware/esp32s3/env.sh ]]; then
            build_esp32s3
        fi
        ;;
    *)
        echo "Unknown target: $target  (expected: rp2040 | host-tests | esp32s3 | all)" >&2
        exit 2
        ;;
esac
