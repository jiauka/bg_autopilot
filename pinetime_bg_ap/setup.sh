#!/usr/bin/env bash
# Sets up the InfiniTime 1.14.0 build environment and integrates the BgAp app.
set -euo pipefail

INFINITIME_TAG="1.14.0"
INFINITIME_DIR="$(pwd)/infinitime"
DOCKER_IMAGE="infinitime/infinitime-build:latest"

echo "=== PineTime BG Autopilot — build environment setup ==="

# ── Prerequisites ──────────────────────────────────────────────────────
for cmd in git docker python3; do
  command -v "$cmd" >/dev/null || { echo "ERROR: $cmd not found"; exit 1; }
done

# Optional flash tools
command -v openocd    >/dev/null && echo "[ok] openocd found"    || echo "[warn] openocd not found (needed for SWD flash)"
command -v nrfutil    >/dev/null && echo "[ok] nrfutil found"    || echo "[warn] nrfutil not installed (needed for OTA DFU)"
command -v adafruit-nrfutil >/dev/null && echo "[ok] adafruit-nrfutil found" || true

# ── Clone InfiniTime ───────────────────────────────────────────────────
if [[ -d "$INFINITIME_DIR/.git" ]]; then
  echo "[skip] InfiniTime already cloned at $INFINITIME_DIR"
else
  echo "Cloning InfiniTime $INFINITIME_TAG …"
  git clone --depth 1 --branch "$INFINITIME_TAG" \
    https://github.com/InfiniTimeOrg/InfiniTime.git \
    "$INFINITIME_DIR"
  echo "Initialising submodules (lvgl, littlefs, …) …"
  git -C "$INFINITIME_DIR" submodule update --init --recursive
fi

# ── Pull Docker build image ────────────────────────────────────────────
echo "Pulling Docker build image …"
docker pull "$DOCKER_IMAGE"

# ── Integrate our app into InfiniTime ─────────────────────────────────
echo "Integrating BgAp app …"
python3 integrate.py "$INFINITIME_DIR"

echo ""
echo "Setup complete. Run ./build.sh to compile."
