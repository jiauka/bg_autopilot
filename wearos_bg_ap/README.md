# B&G AP WearOS App

Wear OS companion app for the ESP32-C3 B&G/Simrad autopilot keypad emulator.

## Requirements

- Wear OS 3.0+ (API 30+) watch
- Android Studio Hedgehog or newer
- ESP32-C3 running the `bg_autopilot` firmware, powered and on the N2K bus

## What it shows

```
┌─────────────────────────┐
│   [Heading compass]     │  ← Animated compass rose
│      ↑ 247.3°           │  ← Vessel heading (large)
│    ▶ 245.0°             │  ← AP commanded heading (yellow)
│       [AUTO]            │  ← Mode badge with pulse animation
│                         │
│   [-1°]  [+1°]          │
│   [-10°] [+10°]         │
│  [AUTO] [WIND] [NAV]    │
│      [STANDBY]          │
│   N2K addr: 0x03        │
└─────────────────────────┘
```

## Compass

- **White arrow** = vessel heading (always pointing up, rose rotates)
- **Yellow dot/line** = AP commanded heading (dashed spoke)
- **Blue dot** = North marker (rotates with the rose)

## BLE connection

The app auto-scans for a device named `BG_AP` on launch.  
On disconnect it automatically retries after 3 seconds.

## Build

```bash
# In Android Studio:
# File → Open → select wearos_bg_ap/
# Run on device (USB or ADB over WiFi)

# Or from command line:
./gradlew assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

## File structure

```
app/src/main/java/com/bgap/wear/
  BleApService.kt   – BLE scan, connect, GATT, notify parsing
  MainViewModel.kt  – ViewModel bridging BLE service to UI
  MainActivity.kt   – Entry point, permission handling
  BgApApp.kt        – Full Compose UI (compass, buttons, readouts)
```

## BLE protocol (matches ESP32 ble_ap.h)

**Service UUID:** `12340001-1234-1234-1234-123456789ABC`

| UUID suffix | Name | Properties |
|---|---|---|
| `...0002...` | CMD | Write / Write-no-response |
| `...0003...` | STATUS | Read + Notify |

**Status packet (12 bytes, notified ~1 Hz + on change):**

| Byte | Content |
|------|---------|
| 0 | Mode (0=Standby 1=Auto 2=NFU 3=Wind 4=Nav) |
| 1-2 | Vessel heading × 10, uint16 LE |
| 3-4 | AP commanded heading × 10, uint16 LE |
| 5 | Data valid (1 = live N2K data) |
| 6 | AP N2K address |
| 7-11 | Reserved |

**Command bytes:**

| Byte | Action |
|------|--------|
| 0x01 | Standby |
| 0x02 | Auto |
| 0x03 | Wind |
| 0x04 | Nav |
| 0x05 | +1° |
| 0x06 | -1° |
| 0x07 | +10° |
| 0x08 | -10° |
| 0x09 | Mode cycle |
