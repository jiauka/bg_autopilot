/**
 * @file autopilot.c
 * @brief B&G/Simrad autopilot state machine
 *
 * Sends commands via PGN 65314 (Simnet AP command).
 * Receives AP state from:
 *   PGN 65341  – Simnet Autopilot Mode
 *   PGN 127237 – Heading/Track Control (commanded heading)
 *   PGN 127250 – Vessel Heading (actual heading)
 */

#include "autopilot.h"
#include "nmea2000.h"
#if CONFIG_BG_AP_CMD_BLE
#include "ble_ap.h"
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>

static const char* TAG = "AP";

static ap_state_t s_state;
static SemaphoreHandle_t s_mutex;

/* ── Simnet pilot mode byte → our enum ─────────────────────────────── */
/*
 * canboat LOOKUP_SIMRAD_PILOT_MODE:
 *   0 = Standby
 *   1 = Auto, compass commanded
 *   2 = Non Follow Up, hand commanded
 *   3 = Wind Mode
 *   4 = Track Mode
 *   5 = No Drift, COG referenced
 */
static ap_mode_t simnet_mode_to_ap(uint8_t raw)
{
  /* Verified from PGN 65341 bus capture on this AP */
  switch (raw) {
    case 0x02:
      return AP_MODE_STANDBY;
    case 0x03:
      return AP_MODE_AUTO;
    case 0x04:
      return AP_MODE_WIND; /* assumed – need capture to confirm */
    case 0x05:
      return AP_MODE_NAV; /* assumed – need capture to confirm */
    default:
      return AP_MODE_UNKNOWN;
  }
}

static const char* mode_name(ap_mode_t m)
{
  switch (m) {
    case AP_MODE_STANDBY:
      return "STANDBY";
    case AP_MODE_AUTO:
      return "AUTO";
    case AP_MODE_NFU:
      return "NFU";
    case AP_MODE_WIND:
      return "WIND";
    case AP_MODE_NAV:
      return "NAV";
    default:
      return "UNKNOWN";
  }
}

/* ── Init ───────────────────────────────────────────────────────────── */
esp_err_t ap_init(void)
{
  s_mutex = xSemaphoreCreateMutex();
  s_state = (ap_state_t){
    .mode = AP_MODE_STANDBY,
    .commanded_heading = 0.0f,
    .dst_addr = AP_DEFAULT_DST_ADDR,
    .ap_status_valid = false,
    .ap_mode = AP_MODE_UNKNOWN,
    .ap_heading = 0.0f,
    .awa = 0.0f,
    .twa=0.0f,
    .vessel_heading = 0.0f,
    .ap_src_addr = 0xFF,
    .last_update_ms = 0,
  };
  ESP_LOGI(TAG, "AP controller ready  dst=0x%02X", s_state.dst_addr);
  return ESP_OK;
}
  
ap_state_t* ap_get_state(void)
{
  return &s_state;
}

void ap_set_dst_addr(uint8_t addr)
{
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_state.dst_addr = addr;
  xSemaphoreGive(s_mutex);
  ESP_LOGI(TAG, "AP dst addr → 0x%02X", addr);
}

/* ── RX callbacks (called from N2K RX task) ────────────────────────── */


void ap_set_mode(ap_mode_t m)
{
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_state.last_update_ms = (uint32_t)(esp_timer_get_time() / 1000);
  xSemaphoreGive(s_mutex);
  printf("ap_set_mode **************************** %d\n",m);
  s_state.ap_status_valid = true;
  s_state.ap_mode=m;
#if CONFIG_BG_AP_CMD_BLE
  ble_ap_notify_status();
#endif
}

#if 0      

/*
 * PGN 65341 – Simnet: Autopilot Mode
 * Fields (after mfg/reserved/industry header, bytes 0-1):
 *   Byte 2 (status1) : 0x10 = Pilot ON
 *   Byte 3 (mode)    : LOOKUP_SIMRAD_PILOT_MODE value
 */
void ap_on_simnet_mode(uint8_t src, uint8_t status1, uint8_t mode_raw)
{
  ap_mode_t m = simnet_mode_to_ap(mode_raw);
  bool pilot_on = (status1 == 0x10);
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_state.ap_mode = m;
  s_state.ap_src_addr = src;
  s_state.dst_addr = src; /* keep command target in sync with AP address */
  s_state.ap_status_valid = true;
  s_state.last_update_ms = (uint32_t)(esp_timer_get_time() / 1000);
  if (!pilot_on)
    s_state.ap_mode = AP_MODE_STANDBY;
  xSemaphoreGive(s_mutex);

//    ESP_LOGI(TAG, "PGN 65341  AP mode=%s (%s)  src=0x%02X",        mode_name(m), pilot_on ? "ON" : "OFF", src);
#if CONFIG_BG_AP_CMD_BLE
  ble_ap_notify_status();
#endif
}
#endif

/*
 * PGN 127237 – Heading/Track Control, field 10 = Heading-To-Steer (commanded)
 * Resolution: 0.0001 rad → convert to degrees here.
 */
void ap_on_heading_track(uint8_t src, float commanded_hdg_deg)
{
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_state.ap_heading = commanded_hdg_deg;
  s_state.ap_src_addr = src;
  s_state.last_update_ms = (uint32_t)(esp_timer_get_time() / 1000);
  xSemaphoreGive(s_mutex);
  ESP_LOGD(TAG, "PGN 127237  Commanded hdg=%.1f°  src=0x%02X", commanded_hdg_deg, src);
  //  printf( "PGN 127237  Commanded hdg=%.1f°  src=0x%02X\n", commanded_hdg_deg, src);
}

/*
 * PGN 127250 – Vessel Heading, field 1 = Heading
 * Resolution: 0.0001 rad → convert to degrees here.
 */
void ap_on_vessel_heading(uint8_t src, float heading_deg)
{
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_state.vessel_heading = heading_deg;
  xSemaphoreGive(s_mutex);
  ESP_LOGD(TAG, "PGN 127250  Vessel hdg=%.1f°  src=0x%02X", heading_deg, src);
  //  printf( "PGN 127250  Vessel hdg=%.1f°  src=0x%02X\n", heading_deg, src);

#if CONFIG_BG_AP_CMD_BLE
  ble_ap_notify_status();
#endif
}

/*
 * PGN 130851 – Simnet: Event Reply: AP command
 * The AP echoes every accepted command (from any keypad) on 130851.
 * We use this to track the actual AP mode regardless of which keypad
 * issued the command.
 */
void ap_on_ap_reply(uint8_t src, uint8_t event, uint8_t direction)
{
  ESP_LOGI(TAG, "PGN 130851  AP reply  event=0x%02X dir=%d  src=0x%02X", event, direction, src);
  xSemaphoreTake(s_mutex, portMAX_DELAY);

  s_state.ap_src_addr = src;
  s_state.ap_status_valid = true;

  switch (event) {
    case SIMNET_AP_CMD_AUTO:
      s_state.mode = AP_MODE_AUTO;
        ESP_LOGI(TAG, "Mode → AUTO (130851 echo)");
      break;
    case SIMNET_AP_CMD_STANDBY:
    case 0xff:  
      s_state.mode = AP_MODE_STANDBY;
      ESP_LOGI(TAG, "Mode → STANDBY (130851 echo)");
      break;
    case SIMNET_AP_CMD_MODE:
          ESP_LOGI(TAG, "Mode → ????NDBY (130851 echo)");
#if 0
      /* MODE cycles the engaged mode: AUTO→WIND→NAV→AUTO */
      switch (s_state.mode) {
        case AP_MODE_AUTO:
          s_state.mode = AP_MODE_WIND;
          break;
        case AP_MODE_WIND:
          s_state.mode = AP_MODE_NAV;
          break;
        case AP_MODE_NAV:
          s_state.mode = AP_MODE_AUTO;
          break;
        default:
          break;
      }
#endif
      ESP_LOGI(TAG, "Mode ----> cycle → %s (130851 echo)", mode_name(s_state.mode));
      break;
    default:
      break;
  }

  xSemaphoreGive(s_mutex);
}

/* ── Command functions ──────────────────────────────────────────────── */

esp_err_t ap_standby(void)
{
  ESP_LOGI(TAG, "Engage STANDBY");
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_state.mode = AP_MODE_STANDBY;
  s_state.ap_mode = AP_MODE_STANDBY;
  s_state.ap_status_valid = true;
  uint8_t ap = s_state.dst_addr;
  xSemaphoreGive(s_mutex);
  return n2k_send_ap_command(SIMNET_AP_CMD_STANDBY, 0, 0, ap);
}

esp_err_t ap_engage_auto(void)
{
  ESP_LOGI(TAG, "Engage AUTO");
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_state.mode = AP_MODE_AUTO;
  s_state.ap_mode = AP_MODE_AUTO;
  s_state.ap_status_valid = true;
  uint8_t ap = s_state.dst_addr;
  xSemaphoreGive(s_mutex);
  return n2k_send_ap_command(SIMNET_AP_CMD_AUTO, 0, 0, ap);
}

esp_err_t ap_engage_wind(void)
{
  ESP_LOGI(TAG, "Engage WIND (AUTO then MODE)");
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_state.mode = AP_MODE_WIND;
  s_state.ap_status_valid = true;
  uint8_t ap = s_state.dst_addr;
  xSemaphoreGive(s_mutex);
  /* No direct wind button on B&G – engage AUTO then cycle to WIND */
  n2k_send_ap_command(SIMNET_AP_CMD_AUTO, 0, 0, ap);
  vTaskDelay(pdMS_TO_TICKS(300));
  return n2k_send_ap_command(SIMNET_AP_CMD_MODE, 0, 0, ap);
}

esp_err_t ap_engage_nav(void)
{
  ESP_LOGI(TAG, "Engage NAV (AUTO then MODE x2)");
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_state.mode = AP_MODE_NAV;
  s_state.ap_status_valid = true;
  uint8_t ap = s_state.dst_addr;
  xSemaphoreGive(s_mutex);
  n2k_send_ap_command(SIMNET_AP_CMD_AUTO, 0, 0, ap);
  vTaskDelay(pdMS_TO_TICKS(300));
  n2k_send_ap_command(SIMNET_AP_CMD_MODE, 0, 0, ap);
  vTaskDelay(pdMS_TO_TICKS(300));
  return n2k_send_ap_command(SIMNET_AP_CMD_MODE, 0, 0, ap);
}

esp_err_t ap_cycle_mode(void)
{
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  uint8_t ap = s_state.dst_addr;
  xSemaphoreGive(s_mutex);
  ESP_LOGI(TAG, "Mode cycle");
  return n2k_send_ap_command(SIMNET_AP_CMD_MODE, 0, 0, ap);
}

esp_err_t ap_adjust_heading(int delta)
{
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  ap_mode_t mode = s_state.mode; /* use local commanded state – 65341 parse unreliable */
  uint8_t ap = s_state.dst_addr;
  xSemaphoreGive(s_mutex);

  if (mode != AP_MODE_AUTO && mode != AP_MODE_WIND) {
    ESP_LOGW(TAG, "Heading adjust ignored – AP mode=%s", mode_name(mode));
    return ESP_ERR_INVALID_STATE;
  }

  uint8_t dir = (delta > 0) ? SIMNET_AP_DIR_STBD : SIMNET_AP_DIR_PORT;
  /* Angle in 0.0001 rad units: 1° → 175, 10° → 1745 */
  uint16_t angle = (uint16_t)roundf(fabsf((float)delta) * (float)M_PI / 180.0f * 10000.0f);

  ESP_LOGI(TAG, "Heading %+d° (0.0001rad=%u)", delta, angle);
  return n2k_send_ap_command(SIMNET_AP_CMD_COURSE, dir, angle, ap);
}
