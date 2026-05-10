/**
 * @file autopilot.h
 * @brief B&G / Simrad NMEA 2000 autopilot controller
 *
 * Commands: PGN 130850 "Simnet: Event Command: AP command"
 * Status received from AP via:
 *   PGN 65341  – Simnet: Autopilot Mode    (mode + status bytes)
 *   PGN 127237 – Heading/Track Control     (commanded heading, field 10)
 *   PGN 127250 – Vessel Heading            (actual compass heading)
 *   PGN 130851 – Simnet: Event Reply       (ACK to our 130850 commands)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define AP_DEFAULT_DST_ADDR 0x0A /* B&G AP computer is typically at N2K addr 10 */

/* AP mode – mirrors Simrad LOOKUP_SIMRAD_PILOT_MODE from canboat */
typedef enum
{
  AP_MODE_STANDBY = 0, /* 0 = Standby                     */
  AP_MODE_AUTO = 1,    /* 1 = Auto, compass commanded      */
  AP_MODE_NFU = 2,     /* 2 = Non Follow-Up, hand command  */
  AP_MODE_WIND = 3,    /* 3 = Wind mode                    */
  AP_MODE_NAV = 4,     /* 4 = Track mode                   */
  AP_MODE_UNKNOWN = 0xFF,
} ap_mode_t;

typedef struct
{
  /* Commanded / local state */
  ap_mode_t mode;          /* Mode we last sent                    */
  float commanded_heading; /* Heading we last commanded (degrees)  */
  uint8_t dst_addr;        /* Autopilot N2K address                */

  /* Status received FROM the autopilot (updated by RX callbacks) */
  bool ap_status_valid;    /* True once we've received PGN 65341   */
  ap_mode_t ap_mode;       /* Mode reported by AP (PGN 65341)      */
  float ap_heading;        /* Commanded heading from AP (PGN 127237, deg) */
    float awa;              /* Apparent wind angle */

  float ap_wind_heading;   /* Commanded wind heading from AP (PGN 127237, deg) */
  float vessel_heading;    /* Actual compass heading (PGN 127250, deg)    */
  float twa;               /* Actual wind heading (PGN 127250, deg)    */
  uint8_t ap_src_addr;     /* N2K address the AP is responding from */
  uint32_t last_update_ms; /* esp_timer ms of last AP status update */
} ap_state_t;

#ifdef __cplusplus
extern "C"
{
#endif

  esp_err_t ap_init(void);
  ap_state_t* ap_get_state(void);
  void ap_set_dst_addr(uint8_t addr);

  /** Engage AUTO heading-hold mode. */
  esp_err_t ap_engage_auto(void);

  /** Engage WIND (apparent wind angle) mode. */
  esp_err_t ap_engage_wind(void);

  /** Engage NAV (track/route) mode. */
  esp_err_t ap_engage_nav(void);

  /** Engage STANDBY (disengage) mode. */
  esp_err_t ap_standby(void);

  /** Cycle: STANDBY → AUTO → WIND → NAV → STANDBY … */
  esp_err_t ap_cycle_mode(void);

  /**
   * Adjust commanded heading by delta degrees (±1 or ±10 only).
   * Only valid in AUTO mode.
   */
  esp_err_t ap_adjust_heading(int delta);

  /* ── Callbacks called from the N2K RX task ──────────────────────────
   * These are called by nmea2000.c when relevant PGNs are received.
   * Do not call directly.
   */
  void ap_on_simnet_mode(uint8_t src, uint8_t status1, uint8_t mode_raw);
  void ap_on_heading_track(uint8_t src, float commanded_hdg_deg);
  void ap_on_vessel_heading(uint8_t src, float heading_deg);
  void ap_on_ap_reply(uint8_t src, uint8_t event, uint8_t direction);
  void ap_set_mode(ap_mode_t mode);

#ifdef __cplusplus
}
#endif
