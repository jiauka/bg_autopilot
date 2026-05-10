/**
 * @file ble_ap.h
 * @brief BLE GATT server for B&G autopilot control from WearOS
 *
 * Advertising name: "BG_AP"
 *
 * Custom service UUID:  12340001-1234-1234-1234-123456789ABC
 *
 * Characteristics:
 *
 *   CMD  12340002-...  Write (no response)
 *        WearOS writes a single byte command:
 *          0x01  Standby
 *          0x02  Auto
 *          0x03  Wind
 *          0x04  Nav
 *          0x05  +1°
 *          0x06  -1°
 *          0x07  +10°
 *          0x08  -10°
 *          0x09  Mode cycle
 *
 *   STATUS  12340003-...  Notify (read + notify)
 *        ESP32 pushes a 12-byte status packet when AP state changes:
 *          Byte  0     : AP mode  (0=Standby 1=Auto 2=NFU 3=Wind 4=Nav 0xFF=Unknown)
 *          Bytes 1-2   : Vessel heading  (uint16 LE, degrees × 10)
 *          Bytes 3-4   : AP commanded heading  (uint16 LE, degrees × 10)
 *          Byte  5     : AP status valid  (0=no data, 1=live)
 *          Byte  6     : AP N2K address
 *          Bytes 7-11  : Reserved / padding (0x00)
 */
#pragma once

#include "esp_err.h"

/* BLE command byte values */
#define BLE_CMD_STANDBY 0x01
#define BLE_CMD_AUTO 0x02
#define BLE_CMD_WIND 0x03
#define BLE_CMD_NAV 0x04
#define BLE_CMD_PLUS_1 0x05
#define BLE_CMD_MINUS_1 0x06
#define BLE_CMD_PLUS_10 0x07
#define BLE_CMD_MINUS_10 0x08
#define BLE_CMD_MODE_CYCLE 0x09
#define BLE_CMD_LOG_TOGGLE 0x0A /* Toggle Actisense N2K bus log on console */

/**
 * Initialise NimBLE stack, register GATT service, start advertising.
 * Call after ap_init().
 */
esp_err_t ble_ap_init(void);

/**
 * Push current AP state to connected WearOS client via notify.
 * Safe to call from any task; no-op if no client is subscribed.
 */
void ble_ap_notify_status(void);
