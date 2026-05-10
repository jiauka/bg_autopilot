/**
 * @file fake_ap.h
 * @brief Virtual Simrad AP computer on the N2K bus (test/simulation).
 *
 * Claims its own ISO address (preferred 0x03) and periodically
 * broadcasts the standard Simnet / NMEA 2000 autopilot PGNs:
 *
 *   PGN  65340 – Simnet: AP Control/Status          1 s
 *   PGN  65302 – Simnet: AC12 State                 1 s
 *   PGN  65341 – Simnet: Autopilot Mode             1 s
 *   PGN 127237 – Heading/Track Control            500 ms
 *   PGN 127250 – Vessel Heading                   500 ms
 *   PGN 127245 – Rudder                           200 ms
 *   PGN 130860 – Simnet: AP Control (device info)   1 s
 *   PGN 126993 – Heartbeat                         60 s
 *
 * Also responds to PGN 130850 keypad commands (Standby / Auto /
 * Mode / Change Course) addressed to its N2K address or broadcast.
 *
 * Device identity: AP Simulator
 *   Manufacturer  : 1857  (Navico / Simrad / B&G)
 *   Device function: 150  (Steering Control)
 *   Device class   :  40  (Navigation System)
 *   Unique ID      : 1
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /** Initialise, claim address, register RX handler, start TX task. */
  esp_err_t fake_ap_init(void);

  /** Return the currently claimed N2K source address. */
  uint8_t fake_ap_get_addr(void);

  /**
   * Handle a received PGN 130850 "AP command" frame.
   * Registered via n2k_set_ap_command_handler() – do not call directly.
   * @param src  Source address of the sender
   * @param dst  Destination address (0xFF = broadcast)
   * @param data Reassembled fast-packet payload (12 bytes)
   * @param len  Payload length
   */
  void fake_ap_on_ap_command(uint8_t src, uint8_t dst, const uint8_t* data, uint8_t len);

#ifdef __cplusplus
}
#endif
