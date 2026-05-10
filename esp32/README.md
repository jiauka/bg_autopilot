# B&G / Simrad NMEA 2000 Autopilot Keypad Emulator

ESP32-C3 firmware that emulates a **B&G Triton 2 autopilot keypad** on an
NMEA 2000 bus using the ESP-IDF 5.x **TWAI** (CAN) driver.  No Arduino.

---

## Hardware

### ESP32-C3 CAN transceiver wiring

```
ESP32-C3 GPIO4  ──► TXD  ┐
ESP32-C3 GPIO5  ◄── RXD  │  SN65HVD230 (or MCP2551 / TJA1050)
ESP32-C3 3V3    ──► VCC  │  3.3 V-tolerant transceiver recommended
ESP32-C3 GND    ──► GND  ┘
                    CANH ──► NMEA 2000 backbone CANH (pin 4)
                    CANL ──► NMEA 2000 backbone CANL (pin 5)
```

**NMEA 2000 Micro-C connector pinout**

| Pin | Signal  |
|-----|---------|
|  1  | Shield  |
|  2  | NET-S (12 V positive) |
|  3  | NET-C (12 V negative / GND) |
|  4  | NET-H (CAN-H) |
|  5  | NET-L (CAN-L) |

> The ESP32-C3 is **not** bus-powered; connect 3.3 V / GND from your
> own regulator.  Do **not** connect the ESP32-C3 directly to the 12 V
> NMEA 2000 power pins.

### UART (serial command interface)

The serial command interface uses **UART0** (USB-CDC on most DevKit boards):

| Signal | GPIO |
|--------|------|
| TX     | 21   |
| RX     | 20   |

Connect a USB-serial adapter or use the built-in USB-CDC at **115200 8N1**.

---

## Build & Flash (IDF 5.x)

```bash
# Set target
idf.py set-target esp32c3

# Build
idf.py build

# Flash & monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

---

## Serial commands

| Key | Action |
|-----|--------|
| `a` | Engage AUTO mode |
| `m` | Cycle mode: STANDBY → AUTO → WIND → NAV → … |
| `+` | Heading +1° (starboard) |
| `-` | Heading −1° (port) |
| `t` | Heading +10° (starboard) |
| `T` | Heading −10° (port) |
| `?` | Show current AP state |
| `h` | Help |

---

## NMEA 2000 protocol notes

### PGN 126208 – Command Group Function (AP key emulation)

This is the primary mechanism used to emulate autopilot keypad presses.
The firmware sends **PGN 126208** (Command Group Function) with the target
PGN 127237 (Heading/Track Control).  Key presses are encoded as command
fields with index 200-209 mapping to key codes 0-9.

```
Byte 0   : Function code = 0x01 (Command)
Byte 1-3 : Target PGN = 127237 LE (0x15F101)
Byte 4   : Priority = 0x08
Byte 5   : Parameter count = 0x01
Byte 6   : Field index 200-209 (encodes AP key)
Byte 7-8 : Field value / key code
```

This is transmitted as a **fast-packet** message.

### PGN 127501 – Navico proprietary AP commands (alternative)

Modern Triton 2 and H5000 systems also respond to manufacturer-specific
PGN 127501 for AP commands. This is implemented as an alternative approach
if needed.

### Key codes (BG_KEY_* defines)

```
BG_KEY_STANDBY  (0x00) – Disengage AP
BG_KEY_AUTO     (0x01) – Engage AUTO heading mode
BG_KEY_WIND     (0x02) – Wind trim mode
BG_KEY_NAV      (0x03) – Navigation mode
BG_KEY_MINUS_1  (0x04) – Decrease heading 1°
BG_KEY_PLUS_1   (0x05) – Increase heading 1°
BG_KEY_MINUS_10 (0x06) – Decrease heading 10°
BG_KEY_PLUS_10  (0x07) – Increase heading 10°
```

### PGN 126208 command structure

The Command Group Function uses a flexible parameter list.  For AP key
emulation, we encode the key as a special field index (200-209) so the
AP firmware recognizes it as a keypad event rather than a direct heading
command.

### Address claim (ISO 11783-5)

The device claims address **0x71** (configurable in `nmea2000.h`).
If a conflict is detected the address is incremented and a new claim
is transmitted.

### Fast-packet framing

All PGNs > 8 bytes use the N2K fast-packet protocol:

```
Frame 0 :  [seq|0x00] [total_len] [data 0-5]
Frame N :  [seq|N]    [data 0-6]
```

Sequence counter is per-PGN, 3-bit wrapping.

---

## Configuration

Edit `main/nmea2000.h` to change:

| Define | Default | Description |
|--------|---------|-------------|
| `TWAI_TX_GPIO` | 4 | CAN TX GPIO |
| `TWAI_RX_GPIO` | 5 | CAN RX GPIO |
| `N2K_PREFERRED_ADDR` | 0x71 | Preferred N2K source address |
| `N2K_MANUFACTURER_CODE` | 144 | Navico/B&G manufacturer code |

Edit `main/autopilot.h` to change:

| Define | Default | Description |
|--------|---------|-------------|
| `AP_DEFAULT_DST_ADDR` | 0xFF | AP controller N2K address (0xFF = broadcast) |

---

## File structure

```
bg_autopilot/
├── CMakeLists.txt
├── sdkconfig.defaults
├── README.md
└── main/
    ├── CMakeLists.txt
    ├── main.c          – app_main, startup, heartbeat timer
    ├── nmea2000.h/.c   – N2K/TWAI stack, fast-packet, address claim
    ├── autopilot.h/.c  – AP state machine, key translation
    └── serial_cmd.h/.c – UART command parser
```

---

## Known limitations / TODO

- The AP destination address defaults to **broadcast (0xFF)**.  For
  cleaner bus behaviour, sniff the bus and set `AP_DEFAULT_DST_ADDR`
  to the actual AP computer address.
- Wind and NAV modes send the correct key but do not track wind angle
  or waypoint data – extend `autopilot.c` if you need that.
- No persistent storage of AP state across reboots (NVS hooks are
  present but not used).
- Tested against B&G H5000/Triton 2 firmware.  Simrad AC42/IS42
  uses the same Navico N2K dialect and should work identically.
# bg_autopilot


# R1

rigth key
I (86609) JOY: RX attr=23
I (86609) JOY: HID[1]: 20 00 00 00 00 00 00 00
I (86924) JOY: RX attr=23
I (86924) JOY: HID[1]: 00 00 00 00 00 00 00 00

left key
I (105329) JOY: RX attr=23
I (105329) JOY: HID[1]: 10 00 00 00 00 00 00 00
I (105659) JOY: RX attr=23
I (105659) JOY: HID[1]: 00 00 00 00 00 00 00 00

UP key
I (123269) JOY: RX attr=23
I (123269) JOY: HID[1]: 01 00 00 00 00 00 00 00
I (123584) JOY: RX attr=23
I (123584) JOY: HID[1]: 00 00 00 00 00 00 00 00
DOWN key
I (136394) JOY: RX attr=23
I (136394) JOY: HID[1]: 02 00 00 00 00 00 00 00
I (136544) JOY: RX attr=23
I (136544) JOY: HID[1]: 00 00 00 00 00 00 00 00
back key
I (272669) JOY: RX attr=31
I (272669) JOY: HID[2]: 02 50 00 00 00 00 00 00
I (272670) JOY: RX attr=31
I (272673) JOY: HID[2]: 00 50 00 00 00 00 00 00

A key
I (210929) JOY: RX attr=23
I (210929) JOY: HID[1]: 04 00 00 00 00 00 00 00
I (210930) JOY: RX attr=23
I (210930) JOY: HID[1]: 00 00 00 00 00 00 00 00

C key

I (235634) JOY: RX attr=23
I (235635) JOY: HID[1]: 02 00 00 00 00 00 00 00
I (235635) JOY: RX attr=23
I (235635) JOY: HID[1]: 00 00 00 00 00 00 00 00

D key

I (250799) JOY: RX attr=23
I (250799) JOY: HID[1]: 01 00 00 00 00 00 00 00
I (250800) JOY: RX attr=23
I (250800) JOY: HID[1]: 00 00 00 00 00 00 00 00

long rigth key

A short rigth is +1, a short left -1, a long R +10, long left -10
"A" key is auto
"C" key is "NAV"
"D" key is "WIND"
back key is STANDBY


● Bash(perl -i -pe 's/^( +)/" " x (length($1)\/2)/e' /home/jaume/work_hobby/ticwatch/bg_autopilot/main/ble_joystick.c)
