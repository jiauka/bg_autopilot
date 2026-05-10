/**
 * @file nmea2000.h
 * @brief NMEA 2000 API for B&G autopilot keypad emulation.
 *
 * Backed by ttlappalainen/NMEA2000 + skarlsson/NMEA2000_twai (nmea2000.cpp).
 *
 * Simnet PGN 130850 payload (12 bytes, fast-packet) – verified by bus capture:
 *   [0-1]  Navico mfg header (0x41, 0x9f)
 *   [2-4]  0x01, 0xff, 0xff
 *   [5]    0x0A  (fixed Simnet field)
 *   [6]    Command (SIMNET_AP_CMD_*)
 *   [7]    0x00
 *   [8]    Direction byte for COURSE; 0xff for AUTO/STANDBY; 0x00 for MODE
 *   [9-10] Angle delta in 0.0001 rad LE for COURSE; 0xff for AUTO/STANDBY
 *   [11]   0x00 (0xff for AUTO/STANDBY)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ── Pin assignments (set via menuconfig: B&G Autopilot) ───────────── */
#define TWAI_TX_GPIO CONFIG_BG_AP_TWAI_TX_GPIO
#define TWAI_RX_GPIO CONFIG_BG_AP_TWAI_RX_GPIO

/* ── N2K bus timing: 250 kbit/s ────────────────────────────────────── */
#define N2K_BITRATE TWAI_TIMING_CONFIG_250KBITS()

/* ── N2K device identity ────────────────────────────────────────────── */
#define N2K_PREFERRED_ADDR 0x71 /* will negotiate on conflict      */
#define N2K_INDUSTRY_CODE 4     /* Marine                           */
#define N2K_DEVICE_FUNCTION 120 /* Display                          */
#define N2K_DEVICE_CLASS 40     /*  Navigation System                           */
/* Identify as Navico/Simrad/B&G (mfg code 1857) so the AP computer
 * accepts our PGN 130850 commands.                                      */
#define N2K_MANUFACTURER_CODE 1857
#define N2K_UNIQUE_ID 0x001234UL

#define N2K_APSIM_INDUSTRY_CODE 4 /* Marine                           */
#define N2K_APSIM_SYSTEM_INSTANCE 0
#define N2K_APSIM_DEVICE_FUNCTION 150 /* Steering Control                 */
#define N2K_APSIM_DEVICE_CLASS 40     /* Navigation System                */
#define N2K_APSIM_UNIQUE_ID 1UL       /* device id 1                      */
#define N2K_APSIM_PREFERRED_ADDR 0x03 /* preferred address on N2K bus     */

/* ── PGNs ────────────────────────────────────────────────────────────  */
#define PGN_ISO_ACK 59392UL
#define PGN_ISO_REQUEST 59904UL
#define PGN_ISO_ADDRESS_CLAIM 60928UL
#define PGN_PRODUCT_INFO 126996UL
#define PGN_CONFIG_INFO 126998UL
#define PGN_HEARTBEAT 126993UL
/* Simnet: AP command (fast-packet, verified by real B&G remote capture) */
#define PGN_SIMNET_AP_COMMAND 130850UL
/* Simnet: keypad keepalive – must be sent ~500 ms or AP ignores commands */
#define PGN_SIMNET_AP_KEEPALIVE 65305UL
/* Simnet: Event Reply (AP → keypad) */
#define PGN_SIMNET_AP_REPLY 130851UL
/* Simnet: Autopilot Mode – broadcast by AP computer ~1 Hz */
#define PGN_SIMNET_AP_MODE 65341UL
/* Standard heading/rudder PGNs broadcast by AP / compass */
#define PGN_HEADING_TRACK 127237UL
#define PGN_VESSEL_HEADING 127250UL
#define PGN_RUDDER 127245UL
/* Other Simnet status PGNs (receive-only) */
#define PGN_SIMNET_AP_STATUS 65340UL
#define PGN_SIMNET_AP_STATE 65302UL
#define PGN_SIMNET_AP_CTRL 130860UL

/* ── PGN 130850 command bytes (payload byte 6) ───────────────────────
 * Verified by bus capture of a real B&G keypad.
 */
#define SIMNET_AP_CMD_AUTO 0x09    /* Engage AUTO – verified capture */
#define SIMNET_AP_CMD_STANDBY 0x06 /* Standby/disengage – verified capture */
#define SIMNET_AP_CMD_MODE 0x0f    /* MODE key – cycles AUTO→WIND→NAV */
#define SIMNET_AP_CMD_COURSE 0x1a  /* Course change – verified capture */

/* ── PGN 65314 direction bytes (payload byte 8, course change only) ──  */
#define SIMNET_AP_DIR_STBD 0x03 /* Starboard / +heading – verified */
#define SIMNET_AP_DIR_PORT 0x02 /* Port      / -heading – verified */

/* ── Public API ─────────────────────────────────────────────────────── */
#ifdef __cplusplus
extern "C"
{
#endif

  esp_err_t n2k_init(void);
  esp_err_t n2k_address_claim(void);
  uint8_t n2k_get_src_addr(void);

  /**
   * Send PGN 130850 "Simnet: AP command" (fast-packet, 12 bytes).
   *
   * @param cmd    SIMNET_AP_CMD_* byte
   * @param dir    SIMNET_AP_DIR_STBD / PORT for COURSE; 0 for other cmds
   * @param angle  Delta in 0.0001 rad for COURSE (1°→175, 10°→1745); 0 otherwise
   * @param ap_addr  Ignored (payload byte[5] is hardcoded 0x0A per Simnet spec)
   */
  esp_err_t n2k_send_ap_command(uint8_t cmd, uint8_t dir, uint16_t angle, uint8_t ap_addr);

  /** ~500 ms Simnet keypad keepalive (PGN 65305). Must be called periodically. */
  esp_err_t n2k_send_simnet_keepalive(void);

  /** 1 Hz heartbeat stub – library sends it automatically. */
  esp_err_t n2k_send_heartbeat(void);

  /** Return the claimed source address for a specific device (0=keypad, 1=AP sim). */
  uint8_t n2k_get_src_addr_dev(int dev);

  /**
   * Send any N2K message via a specific device index.
   * Source address is set automatically from the device's claimed address.
   * Payload > 8 bytes is framed as fast-packet automatically.
   * @param dev  0 = keypad, 1 = AP simulator
   */
  esp_err_t n2k_send_raw(uint8_t priority, uint32_t pgn, uint8_t dst, const uint8_t* data, uint8_t len, int dev);

  /**
   * Register a handler called for every received PGN 130850 AP command.
   * Pass NULL to unregister. Called from the N2K RX task.
   */
  void n2k_set_ap_command_handler(void (*handler)(uint8_t src, uint8_t dst, const uint8_t* data, uint8_t len));

#ifdef __cplusplus
}
#endif
