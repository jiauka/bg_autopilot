#!/usr/bin/env bash
# Builds the InfiniTime firmware with BgAp app using Docker.
# First run downloads the ARM GCC toolchain and nRF5 SDK inside the container (~700 MB).
set -euo pipefail

INFINITIME_DIR="$(cd "$(dirname "$0")/infinitime" && pwd)"
DOCKER_IMAGE="infinitime/infinitime-build:latest"

if [[ ! -d "$INFINITIME_DIR" ]]; then
  echo "ERROR: InfiniTime not found. Run ./setup.sh first."
  exit 1
fi

echo "=== Building PineTime BG Autopilot firmware ==="

# Re-run integration in case src/ files changed
python3 "$(dirname "$0")/integrate.py" "$INFINITIME_DIR"

docker run --rm \
  -v "$INFINITIME_DIR":/sources \
  -e BUILD_TYPE=Release \
  -e DISABLE_POSTBUILD=false \
  "$DOCKER_IMAGE" \
  bash /sources/docker/build.sh

echo ""
echo "=== Build complete ==="
echo "Output: $INFINITIME_DIR/build/output/"
