# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Wear OS companion app for a B&G/Simrad marine autopilot keypad emulator. It controls an ESP32-C3 device (`BG_AP`) via BLE, displaying real-time vessel/AP heading on an animated compass rose and sending autopilot commands.

## Build & Deploy

```bash
# Build debug APK
./gradlew assembleDebug

# Build and install on connected Wear OS watch
./gradlew installDebug

# Install manually via adb
adb install app/build/outputs/apk/debug/app-debug.apk
```

No automated tests exist in this project. Testing requires a physical Wear OS watch (min SDK 30 / Wear OS 3.0+) and the ESP32-C3 BLE peripheral, or BLE simulation.

## Architecture

Single-activity Compose app. Data flows: `BleApService` → `MainViewModel` → `BgApApp` (UI).

| File | Role |
|------|------|
| `BleApService.kt` | BLE scanning, GATT connection, status packet parsing, command dispatch |
| `MainViewModel.kt` | AndroidViewModel bridging BLE service state to Compose UI |
| `BgApApp.kt` | All UI screens (Compose for Wear OS) |
| `ApService.kt` | Interface contract for the BLE service |
| `MainActivity.kt` | Entry point; handles runtime BLE/location permissions |

## BLE Protocol

- **Device name filter:** `BG_AP`
- **Service UUID:** `12340001-1234-1234-1234-123456789ABC`
- **CMD characteristic** (UUID `...0002...`): Write / Write-no-response — sends single-byte command codes
- **STATUS characteristic** (UUID `...0003...`): Read + Notify — 12-byte packets at ~1 Hz

Status packet layout (little-endian):
```
[0]    mode byte (0=STBY, 1=AUTO, 2=WIND, 3=NAV)
[1-2]  vessel heading × 10 (Int16)
[3-4]  AP commanded heading × 10 (Int16)
[5]    data valid flag
[6]    N2K address
[7-11] reserved
```

Command bytes for heading adjustments and mode changes are defined as constants in `BleApService.kt`. Commands are queued with a 100 ms inter-command delay. Auto-reconnect triggers after a 3 s disconnect.

## UI Details

`BgApApp.kt` implements four screens driven by `bleState` and `apStatus` StateFlows:

- **ConnectingScreen** — shown while scanning/connecting
- **ErrorScreen** — shown on failure with a retry button
- **MainScreen** — active control: compass rose + heading readouts + control buttons
- **HeadingCompass** — Canvas-drawn compass; vessel arrow always points up (rose rotates), yellow dashed line shows AP heading; 300 ms tween animations

Mode badge uses a pulsing alpha animation (0.7–1.0, 800 ms cycle). Buttons: ±1°, ±10°, AUTO, WIND, NAV, STANDBY.

## Key Dependencies

- `androidx.wear.compose` — Wear OS Compose foundation and Material
- `com.google.android.horologist:horologist-compose-layout` — Wear OS layout helpers
- `kotlinx.coroutines` — async BLE I/O (SupervisorJob + Dispatchers.IO)
- Compose BOM `2024.06.00`, Wear Compose `1.3.1`, Kotlin `2.0.0`, AGP `8.5.2`

The companion ESP32-C3 firmware lives in a sibling directory (`bg_autopilot`/IDF project) in the same repo; that code is built with ESP-IDF 5.4 (IDF 5.5 has a known TWAI/CAN incompatibility noted in git history).