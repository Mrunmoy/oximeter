#!/usr/bin/env bash
#
# Build OxiNode firmware in a Docker container.
#
# Usage:
#   docker/docker-build.sh rp2040     # → build/rp2040/oxinode.uf2
#   docker/docker-build.sh esp32s3    # → firmware/esp32s3/build/oxinode.bin
#
set -euo pipefail

target="${1:-rp2040}"
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo_root"

if ! docker info >/dev/null 2>&1; then
    echo "[docker-build] cannot reach docker daemon — is your user in the docker group?" >&2
    echo "                Try: sudo usermod -aG docker \$USER && newgrp docker" >&2
    exit 1
fi

case "$target" in
    rp2040)
        image="oxinode-rp2040-builder"
        dockerfile="docker/Dockerfile.rp2040"
        ;;
    esp32s3|esp32)
        image="oxinode-esp32s3-builder"
        dockerfile="docker/Dockerfile.esp32"
        ;;
    *)
        echo "Unknown target: $target  (expected: rp2040 | esp32s3)" >&2
        exit 2
        ;;
esac

echo "[docker-build] building $image from $dockerfile"
docker build -t "$image" -f "$dockerfile" docker/

echo "[docker-build] running build for target=$target"
docker run --rm \
    -v "$repo_root":/project \
    -u "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    "$image"

echo "[docker-build] done — artifacts under build/$target/  (or firmware/esp32s3/build/ for esp32s3)"
