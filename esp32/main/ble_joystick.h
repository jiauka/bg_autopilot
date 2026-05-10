/**
 * @file ble_joystick.h
 * @brief BLE Central HID client for joystick/remote control (Magicsee R1).
 *
 * Scans for a BLE HID device by name (CONFIG_BG_AP_JOYSTICK_DEVICE_NAME),
 * connects, discovers the HID service (0x1812), enables input-report
 * notifications, and maps arrow-key / axis events to autopilot commands:
 *
 *   Right arrow / +X axis  →  ap_adjust_heading(+1)
 *   Left  arrow / -X axis  →  ap_adjust_heading(-1)
 *   Up    arrow            →  ap_adjust_heading(+10)
 *   Down  arrow            →  ap_adjust_heading(-10)
 *
 * Requires BLE (NimBLE) to already be initialised by ble_ap_init().
 * Call ble_joystick_init() after ble_ap_init() in app_main.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the BLE joystick client.
 * Waits for NimBLE to sync, then starts scanning for the configured device.
 * Auto-reconnects on disconnect.
 */
esp_err_t ble_joystick_init(void);

#ifdef __cplusplus
}
#endif
