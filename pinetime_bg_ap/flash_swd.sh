#!/usr/bin/env bash
# Flashes the full firmware (bootloader + app) via SWD / OpenOCD.
# Tested with a J-Link and a CMSIS-DAP (e.g. Black Magic Probe).
# Usage: ./flash_swd.sh [interface]   default interface: jlink
set -euo pipefail

INFINITIME_DIR="$(pwd)/infinitime"
BUILD_DIR="$INFINITIME_DIR/build/src"
INTERFACE="${1:-jlink}"

# Resolve firmware paths
BOOTLOADER="$BUILD_DIR/pinetime-mcuboot-app-image-1.14.0.bin"
APP="$BUILD_DIR/pinetime-app-1.14.0.bin"

# Fall back to glob if exact name doesn't match
[[ -f "$BOOTLOADER" ]] || BOOTLOADER="$(ls "$BUILD_DIR"/pinetime-mcuboot-app-image-*.bin 2>/dev/null | head -1)"
[[ -f "$APP"        ]] || APP="$(ls "$BUILD_DIR"/pinetime-app-*.bin 2>/dev/null | head -1)"

if [[ -z "$BOOTLOADER" || -z "$APP" ]]; then
  echo "ERROR: firmware not found — run ./build.sh first"
  exit 1
fi

echo "=== Flashing PineTime via SWD ($INTERFACE) ==="
echo "  MCUBoot: $BOOTLOADER"
echo "  App:     $APP"

# Address map (PineTime / nRF52832):
#   0x00000000  MCUBoot bootloader (first 24 KB)
#   0x00008000  App image slot 0
OPENOCD_CFG="$(dirname "$0")/openocd_pinetime.cfg"

if [[ ! -f "$OPENOCD_CFG" ]]; then
cat > "$OPENOCD_CFG" <<'EOF'
source [find interface/stlink.cfg]
transport select swd
source [find target/nrf52.cfg]
$_TARGETNAME configure -event reset-init {
  nrf52_recover
}
EOF
fi

openocd -f "$OPENOCD_CFG" \
  -c "init" \
  -c "reset halt" \
  -c "nrf5 mass_erase" \
  -c "program $BOOTLOADER verify 0x00000000" \
  -c "program $APP        verify 0x00008000" \
  -c "reset run" \
  -c "exit"

echo "Flash complete. PineTime should reboot into BG Autopilot."
