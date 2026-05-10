/**
 * @file fake_ap.c
 * @brief Virtual AP computer on the N2K bus (test/simulation).
 *
 * Runs as device index 1 inside the shared NMEA2000 library instance.
 * n2k_send_raw(..., 1) sets the source address to the AP sim's claimed addr.
 * SW loopback: TWAI cannot receive its own frames, so received PGNs are
 * delivered directly to autopilot.c callbacks after each broadcast.
 *
 * Threading:
 *   fap_tx_task snapshots state under s.mutex, releases it, then sends.
 *   fake_ap_on_ap_command is called from n2k_task (which holds s_mutex).
 *   Keeping these mutexes non-nested avoids deadlock.
 */

#include "fake_ap.h"
#include "nmea2000.h"
#include "autopilot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <math.h>

static const char* TAG = "FAP";

#define FAP_DEV 1 /* library device index for the AP simulator */

#define MFG0 0x41u
#define MFG1 0x9fu

typedef enum
{
  FAP_STANDBY = 0,
  FAP_AUTO = 1,
  FAP_WIND = 3,
  FAP_NAV = 4,
} fap_mode_t;

typedef struct
{
  fap_mode_t mode;
  float heading;
  float commanded_hdg;
  float wind_angle;
  SemaphoreHandle_t mutex;
} fap_state_t;

static fap_state_t s;

static void wrap360(float* v)
{
  while (*v < 0.0f)
    *v += 360.0f;
  while (*v >= 360.0f)
    *v -= 360.0f;
}

static uint16_t deg_to_n2k(float deg)
{
  return (uint16_t)(deg * (float)M_PI / 180.0f * 10000.0f);
}

/* ── PGN senders (called without s.mutex held) ──────────────────────── */

static void send_pgn65341(fap_mode_t mode, float wind_angle)
{
  uint8_t status1, mode_raw;
  uint8_t p[8] = { MFG0, MFG1, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF };
  switch (mode) {
    case FAP_AUTO:
      status1 = 0x10;
      mode_raw = 0x01;
      break;
    case FAP_WIND:
      status1 = 0x10;
      mode_raw = 0x03;
      {
        uint16_t w = deg_to_n2k(wind_angle);
        p[4] = w & 0xFF;
        p[5] = w >> 8;
      }
      break;
    case FAP_NAV:
      status1 = 0x10;
      mode_raw = 0x04;
      break;
    default:
      status1 = 0x00;
      mode_raw = 0x00;
      break;
  }
  p[2] = status1;
  p[3] = mode_raw;
  n2k_send_raw(6, PGN_SIMNET_AP_MODE, 0xFF, p, 8, FAP_DEV);
  ap_on_simnet_mode(n2k_get_src_addr_dev(FAP_DEV), status1, mode_raw);
}

static void send_pgn65340(fap_mode_t mode)
{
  uint8_t p[8] = { MFG0, MFG1, 0x00, 0x00, 0xFE, 0xF8, 0x00, 0x80 };
  switch (mode) {
    case FAP_STANDBY:
      p[2] = 0x00;
      p[3] = 0x00;
      p[4] = 0xFE;
      p[5] = 0xF8;
      break;
    case FAP_AUTO:
      p[2] = 0x10;
      p[3] = 0x01;
      p[4] = 0xFE;
      p[5] = 0xFA;
      break;
    case FAP_WIND:
      p[2] = 0x10;
      p[3] = 0x03;
      p[4] = 0xFE;
      p[5] = 0xFA;
      break;
    case FAP_NAV:
      p[2] = 0x10;
      p[3] = 0x06;
      p[4] = 0xFE;
      p[5] = 0xF8;
      break;
  }
  n2k_send_raw(6, PGN_SIMNET_AP_STATUS, 0xFF, p, 8, FAP_DEV);
}

static void send_pgn65302(fap_mode_t mode)
{
  uint8_t p[8] = { MFG0, MFG1, 0x0A, 0x6B, 0x00, 0x00, 0x00, 0xFF };
  switch (mode) {
    case FAP_STANDBY:
      p[3] = 0x6B;
      p[6] = 0x00;
      break;
    case FAP_AUTO:
      p[3] = 0x69;
      p[6] = 0x28;
      break;
    case FAP_WIND:
      p[3] = 0x69;
      p[6] = 0x30;
      break;
    case FAP_NAV:
      p[3] = 0x6B;
      p[6] = 0x28;
      break;
  }
  n2k_send_raw(6, PGN_SIMNET_AP_STATE, 0xFF, p, 8, FAP_DEV);
}

static void send_vessel_heading(float heading)
{
  uint16_t raw = deg_to_n2k(heading);
  uint8_t p[8] = { 0x00, (uint8_t)(raw & 0xFF), (uint8_t)(raw >> 8), 0xFF, 0x7F, 0xFF, 0x7F, 0xFD };
  n2k_send_raw(6, PGN_VESSEL_HEADING, 0xFF, p, 8, FAP_DEV);
  ap_on_vessel_heading(n2k_get_src_addr_dev(FAP_DEV), heading);
}

static void send_heading_track(fap_mode_t mode, float commanded_hdg)
{
  uint8_t p[21];
  memset(p, 0xFF, sizeof(p));
  switch (mode) {
    case FAP_AUTO: {
      p[0] = 0x10;
      uint16_t raw = deg_to_n2k(commanded_hdg);
      p[10] = raw & 0xFF;
      p[11] = raw >> 8;
      break;
    }
    case FAP_NAV:
      p[0] = 0x40;
      break;
    default:
      p[0] = 0xFF;
      break;
  }
  n2k_send_raw(6, PGN_HEADING_TRACK, 0xFF, p, sizeof(p), FAP_DEV);
  if (mode == FAP_AUTO)
    ap_on_heading_track(n2k_get_src_addr_dev(FAP_DEV), commanded_hdg);
}

static void send_rudder(void)
{
  uint8_t p[8] = { 0xFF, 0xF8, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF };
  n2k_send_raw(6, PGN_RUDDER, 0xFF, p, 8, FAP_DEV);
}

static void send_pgn130860(void)
{
  static const uint8_t p[23] = { MFG0, MFG1, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 0x7F };
  n2k_send_raw(6, PGN_SIMNET_AP_CTRL, 0xFF, p, sizeof(p), FAP_DEV);
}

static void send_heartbeat(void)
{
  uint8_t p[8] = { 0xE8, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
  n2k_send_raw(7, PGN_HEARTBEAT, 0xFF, p, 8, FAP_DEV);
}

/* ── TX task ────────────────────────────────────────────────────────── */
static void fap_tx_task(void* arg)
{
  uint32_t tick = 0;
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(100));
    tick++;

    /* Snapshot state before releasing mutex – never hold s.mutex while
     * calling n2k_send_raw (which takes s_mutex) to avoid deadlock with
     * the N2K RX task that holds s_mutex while calling our command handler
     * (which tries to take s.mutex). */
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    fap_mode_t mode = s.mode;
    float heading = s.heading;
    float commanded = s.commanded_hdg;
    float wind = s.wind_angle;
    xSemaphoreGive(s.mutex);

    if (tick % 2 == 0)
      send_rudder();
    if (tick % 5 == 0) {
      send_vessel_heading(heading);
      send_heading_track(mode, commanded);
    }
    if (tick % 10 == 0) {
      send_pgn65340(mode);
      send_pgn65302(mode);
      send_pgn65341(mode, wind);
      send_pgn130860();
    }
    if (tick % 600 == 0)
      send_heartbeat();
  }
}

/* ── AP command handler (called from N2K RX task via s_ap_cmd_handler) */
void fake_ap_on_ap_command(uint8_t src, uint8_t dst, const uint8_t* data, uint8_t len)
{
  uint8_t my_addr = n2k_get_src_addr_dev(FAP_DEV);
  if (dst != 0xFF && dst != my_addr)
    return; /* not addressed to us */
  if (len < 12)
    return;

  uint8_t cmd = data[6];
  uint8_t dir = data[8];
  uint16_t raw_a = (uint16_t)data[9] | ((uint16_t)data[10] << 8);
  /* Angle is in 0.0001 rad units; convert to degrees */
  float delta = (float)raw_a * (180.0f / (float)M_PI) / 10000.0f;

  /* Echo PGN 130851 reply before taking s.mutex (n2k_send_raw uses s_mutex
   * recursively – safe because we're already inside the N2K RX task) */
  n2k_send_raw(6, PGN_SIMNET_AP_REPLY, src, data, len, FAP_DEV);
  ap_on_ap_reply(my_addr, cmd, dir); /* SW loopback */

  xSemaphoreTake(s.mutex, portMAX_DELAY);
  switch (cmd) {
    case SIMNET_AP_CMD_STANDBY:
      s.mode = FAP_STANDBY;
      ESP_LOGI(TAG, "STANDBY  from 0x%02X", src);
      break;
    case SIMNET_AP_CMD_AUTO:
      s.mode = FAP_AUTO;
      s.commanded_hdg = s.heading;
      ESP_LOGI(TAG, "AUTO hdg=%.1f°  from 0x%02X", s.commanded_hdg, src);
      break;
    case SIMNET_AP_CMD_MODE:
      switch (s.mode) {
        case FAP_AUTO:
          s.mode = FAP_WIND;
          break;
        case FAP_WIND:
          s.mode = FAP_NAV;
          break;
        case FAP_NAV:
          s.mode = FAP_AUTO;
          break;
        default:
          break;
      }
      ESP_LOGI(TAG, "MODE → %d  from 0x%02X", (int)s.mode, src);
      break;
    case SIMNET_AP_CMD_COURSE:
      if (s.mode == FAP_AUTO) {
        float d = (dir == SIMNET_AP_DIR_STBD) ? delta : -delta;
        s.commanded_hdg += d;
        wrap360(&s.commanded_hdg);
        s.heading = s.commanded_hdg;
        ESP_LOGI(TAG, "COURSE %+.1f° → %.1f°  from 0x%02X", d, s.commanded_hdg, src);
      }
      break;
    default:
      ESP_LOGD(TAG, "unknown cmd 0x%02X  from 0x%02X", cmd, src);
      break;
  }
  xSemaphoreGive(s.mutex);
}

/* ── Public API ─────────────────────────────────────────────────────── */
esp_err_t fake_ap_init(void)
{
  s.mode = FAP_STANDBY;
  s.heading = 180.0f;
  s.commanded_hdg = 180.0f;
  s.wind_angle = 0.0f;
  s.mutex = xSemaphoreCreateMutex();

  /* Library handles ISO address claim for device 1 (AP Simulator) */
  ESP_LOGI(TAG, "AP Simulator ready (dev1 pref=0x%02X)", N2K_APSIM_PREFERRED_ADDR);

  n2k_set_ap_command_handler(fake_ap_on_ap_command);

  BaseType_t ok = xTaskCreate(fap_tx_task, "fap_tx", 4096, NULL, 4, NULL);
  return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}

uint8_t fake_ap_get_addr(void)
{
  return n2k_get_src_addr_dev(FAP_DEV);
}
