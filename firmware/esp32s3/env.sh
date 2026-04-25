#!/bin/bash
# PHASE 2 — skeleton; first prototype runs on RP2040
#
# Source this file from the firmware/esp32s3/ directory to enter the
# ESP-IDF v5.5 build environment. It expects the IDF tree to live under
# third_party/esp-idf as a git submodule.
#
# First-time setup (host build, no Docker):
#
#   git submodule update --init --recursive third_party/esp-idf
#   cd third_party/esp-idf && ./install.sh esp32s3 && cd -
#   source env.sh
#   idf.py set-target esp32s3
#   idf.py build
#
# If you'd rather not stage the IDF in this tree, use the Docker path
# instead:
#
#   ./docker/docker-build.sh esp32s3
#
# which builds inside espressif/idf:release-v5.5 with no host pollution.

# Resolve absolute path to this script's directory (works under bash
# regardless of cwd at source time).
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

export IDF_PATH="${script_dir}/third_party/esp-idf"
export IDF_TOOLS_PATH="${script_dir}/third_party/esp-idf-tools"

if [ ! -f "${IDF_PATH}/export.sh" ]; then
    echo "[env.sh] IDF_PATH=${IDF_PATH} not initialized." >&2
    echo "         Run: git submodule update --init --recursive third_party/esp-idf" >&2
    echo "         Or use Docker:  ./docker/docker-build.sh esp32s3" >&2
    return 1 2>/dev/null || exit 1
fi

# shellcheck disable=SC1091
. "${IDF_PATH}/export.sh"
