#!/usr/bin/env bash
# OTA flash InfiniTime via BLE using nrfutil (Nordic Secure DFU over MCUboot).
# Usage: ./flash_ota.sh <watch-BLE-address>   e.g. ./flash_ota.sh E4:5A:37:5C:A8:0F
#
# Install nrfutil: pip3 install nrfutil
# Find watch address: timeout 10 sudo bluetoothctl scan on | grep -i pine
set -euo pipefail

INFINITIME_DIR="$(cd "$(dirname "$0")/infinitime" && pwd)"
BUILD_DIR="$INFINITIME_DIR/build/output"
ADDR="${1:-}"

if [[ -z "$ADDR" ]]; then
  echo "Usage: $0 <BLE-address>"
  echo "Find address: timeout 10 sudo bluetoothctl scan on | grep -i pine"
  exit 1
fi

DFU_PKG="$(ls "$BUILD_DIR"/pinetime-mcuboot-app-dfu-*.zip 2>/dev/null | head -1)"
if [[ -z "$DFU_PKG" ]]; then
  echo "ERROR: DFU package not found in $BUILD_DIR — run ./build.sh first"
  exit 1
fi

SCRIPT="$(dirname "$0")/ota_dfu.py"

echo "=== OTA flash to $ADDR ==="
echo "  Package: $DFU_PKG"
echo ""
echo "Uploading (~2-3 min)..."
# bleak uses D-Bus/BlueZ and does not need root
if [[ $EUID -eq 0 ]]; then
  sudo -u "$SUDO_USER" python3 "$SCRIPT" "$DFU_PKG" -a "$ADDR"
else
  python3 "$SCRIPT" "$DFU_PKG" -a "$ADDR"
fi

echo ""
echo "Done. Watch will reboot into new firmware and validate automatically."
