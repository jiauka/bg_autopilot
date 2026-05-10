/**
 * @file ble_joystick.c
 * @brief BLE Central HID client – Magicsee R1 (or any BLE HID remote).
 *
 * Discovery sequence after connecting:
 *   1. Discover HID service (UUID 0x1812)
 *   2. Discover all characteristics in the service
 *   3. For each notify-capable chr, discover descriptors to find its CCCD
 *   4. Write 0x0001 to each CCCD to enable input-report notifications
 */

#include "ble_joystick.h"
#include "autopilot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <string.h>

#include "nimble/nimble_port.h"
#include "os/os_mbuf.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"

static const char *TAG = "JOY";

/* ── Configuration ───────────────────────────────────────────────── */
#define JOY_DEVICE_NAME     CONFIG_BG_AP_JOYSTICK_DEVICE_NAME
#define JOY_RESCAN_MS       5000
#define JOY_CMD_INTERVAL_MS 250

/* R1 characteristic value handles (fixed in R1 firmware) */
#define R1_ATTR_NAV   23   /* nav keys: Right=0x20, Left=0x10, Up/D=0x01, Down/C=0x02, A=0x04 */
#define R1_ATTR_MEDIA 31   /* media: Back=0x02 in byte[0] */

#define LONG_PRESS_US 250000   /* 200 ms in µs (esp_timer unit) */

/* ── Module state ────────────────────────────────────────────────── */
static uint16_t      s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static TimerHandle_t s_rescan_timer;
static int64_t       s_last_cmd_ms;

/* Per-key esp_timers for right, left, up — fire long-press action at 500 ms */
static esp_timer_handle_t s_right_timer;
static esp_timer_handle_t s_left_timer;
static esp_timer_handle_t s_up_timer;
static volatile bool      s_right_long;   /* set when long-press timer fires */
static volatile bool      s_left_long;

static uint8_t  s_prev_nav_bits;

/* Notify-capable characteristics found in the HID service */
#define MAX_NOTIFY_CHRS 8
static struct {
  uint16_t def_handle;
  uint16_t val_handle;
  uint16_t end_handle;
  uint16_t cccd_handle;
} s_chrs[MAX_NOTIFY_CHRS];
static int      s_chr_count;
static int      s_dsc_chr_idx;
static int      s_cccd_en_idx;
static uint16_t s_hid_start;
static uint16_t s_hid_end;
static uint16_t s_hid_cp_handle;
static uint16_t s_report_map_handle;

/* HID service and CCCD UUIDs */
static const ble_uuid16_t s_hid_svc_uuid = BLE_UUID16_INIT(0x1812);
static const ble_uuid16_t s_cccd_uuid    = BLE_UUID16_INIT(0x2902);

/* ── Forward declarations ─────────────────────────────────────────── */
static void start_scan(void);
static void start_dsc_discovery(void);
static void enable_next_cccd(void);
static int  gap_event_cb(struct ble_gap_event *ev, void *arg);

/* ── Rescan timer ────────────────────────────────────────────────── */
static void rescan_timer_cb(TimerHandle_t t)
{
  if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE && ble_hs_synced())
    start_scan();
}

/* ── Advertisement name check ────────────────────────────────────── */
static bool name_matches(const uint8_t *data, uint8_t dlen)
{
  struct ble_hs_adv_fields f;
  if (ble_hs_adv_parse_fields(&f, data, dlen) != 0 || f.name == NULL)
    return false;
  size_t n = strlen(JOY_DEVICE_NAME);
  return (f.name_len >= n) && (memcmp(f.name, JOY_DEVICE_NAME, n) == 0);
}

/* ── BLE scan ────────────────────────────────────────────────────── */
static void start_scan(void)
{
  struct ble_gap_disc_params dp = {
    .passive = 0,
    .filter_duplicates = 1,
  };
  int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &dp,
                         gap_event_cb, NULL);
  if (rc != 0 && rc != BLE_HS_EALREADY)
    ESP_LOGE(TAG, "scan start err %d", rc);
  else
    ESP_LOGI(TAG, "Scanning for \"%s\"", JOY_DEVICE_NAME);
}

/* ── AP command with debounce ─────────────────────────────────────── */
static void cmd_heading(int delta)
{
  int64_t now = esp_timer_get_time() / 1000;
  if (now - s_last_cmd_ms < JOY_CMD_INTERVAL_MS)
    return;
  s_last_cmd_ms = now;
  ap_adjust_heading(delta);
}

/* ── Long-press timer callbacks (fire at 500 ms without release) ──── */
static void right_timer_cb(void *arg)
{
  s_right_long = true;
  cmd_heading(+10);
  ESP_LOGI(TAG, "Right LONG → +10°");
}

static void left_timer_cb(void *arg)
{
  s_left_long = true;
  cmd_heading(-10);
  ESP_LOGI(TAG, "Left LONG → -10°");
}

static void up_timer_cb(void *arg)
{
  ap_engage_wind();
  ESP_LOGI(TAG, "Up LONG → WIND");
}

/* ── HID report parsing ──────────────────────────────────────────── */
static void parse_report(uint16_t attr_handle, const uint8_t *d, int len)
{
  if (len < 1) return;
  esp_timer_stop(s_right_timer);
  esp_timer_stop(s_left_timer);
  esp_timer_stop(s_up_timer);

  if (attr_handle == R1_ATTR_NAV) {
    uint8_t bits    = d[0];
    uint8_t changed = bits ^ s_prev_nav_bits;

    /* Right: timer fires +10° at LONG_PRESS_US ; release before that → +1° */
    if (changed & 0x20) {
      if (bits & 0x20) {
        s_right_long = false;
        esp_timer_start_once(s_right_timer, LONG_PRESS_US);
      } else {
        esp_timer_stop(s_right_timer);
        if (!s_right_long) {
          cmd_heading(+1);
          ESP_LOGI(TAG, "Right SHORT → +1°");
        }
        s_right_long = false;
      }
    }

    /* Left: same pattern */
    if (changed & 0x10) {
      if (bits & 0x10) {
        s_left_long = false;
        esp_timer_start_once(s_left_timer, LONG_PRESS_US);
      } else {
        esp_timer_stop(s_left_timer);
        if (!s_left_long) {
          cmd_heading(-1);
          ESP_LOGI(TAG, "Left SHORT → -1°");
        }
        s_left_long = false;
      }
    }

    /* Up/D: timer fires WIND at LONG_PRESS_US*2 ; short release does nothing */
    if (changed & 0x01) {
      if (bits & 0x01) {
        esp_timer_start_once(s_up_timer, LONG_PRESS_US*2);
      }
    }

    /* A: short press → AUTO, */

    if ((changed & bits) & 0x04) ap_engage_auto();
    /* C/Down: NAV on key-down */
    if ((changed & bits) & 0x02) ap_engage_nav();

    s_prev_nav_bits = bits;

  } else if (attr_handle == R1_ATTR_MEDIA) {
    /* Back: press → STANDBY */
    if (d[0] == 0x02) ap_standby();
  }
}

/* ── Report Map read (diagnostic) ───────────────────────────────── */
static int read_report_map_cb(uint16_t ch, const struct ble_gatt_error *err,
                               struct ble_gatt_attr *attr, void *arg)
{
  if (err->status != 0) {
    ESP_LOGW(TAG, "Report Map read err %d", err->status);
    return 0;
  }
  struct os_mbuf *om = attr->om;
  uint16_t len = OS_MBUF_PKTLEN(om);
  ESP_LOGI(TAG, "Report Map (%d bytes):", len);
  uint8_t buf[64];
  uint16_t off = 0;
  while (off < len) {
    uint16_t chunk = (len - off > sizeof(buf)) ? sizeof(buf) : (len - off);
    os_mbuf_copydata(om, off, chunk, buf);
    char hex[sizeof(buf) * 3 + 1];
    for (uint16_t i = 0; i < chunk; i++)
      sprintf(hex + i * 3, "%02x ", buf[i]);
    hex[chunk * 3] = '\0';
    ESP_LOGI(TAG, "  [%3d] %s", off, hex);
    off += chunk;
  }
  return 0;
}

/* ── CCCD write (enable notifications) ───────────────────────────── */
/* ATT "Insufficient Encryption" (271): skip and continue */
#define ATT_ERR_INSUF_ENC 271

static int write_cccd_cb(uint16_t ch, const struct ble_gatt_error *err,
                          struct ble_gatt_attr *attr, void *arg)
{
  if (err->status != 0 && err->status != ATT_ERR_INSUF_ENC)
    ESP_LOGW(TAG, "CCCD write err %d (chr val=%d)", err->status,
             s_chrs[s_cccd_en_idx].val_handle);
  else if (err->status == ATT_ERR_INSUF_ENC)
    ESP_LOGI(TAG, "CCCD val=%d needs encryption – skipping",
             s_chrs[s_cccd_en_idx].val_handle);
  else
    ESP_LOGI(TAG, "CCCD val=%d subscribed", s_chrs[s_cccd_en_idx].val_handle);
  s_cccd_en_idx++;
  enable_next_cccd();
  return 0;
}

static void enable_next_cccd(void)
{
  if (s_cccd_en_idx >= s_chr_count) {
    if (s_hid_cp_handle) {
      static const uint8_t exit_suspend = 0x01;
      ble_gattc_write_no_rsp_flat(s_conn_handle, s_hid_cp_handle,
                                   &exit_suspend, sizeof(exit_suspend));
      ESP_LOGI(TAG, "HID Control Point Exit Suspend → handle %d", s_hid_cp_handle);
    }
    if (s_report_map_handle)
      ble_gattc_read(s_conn_handle, s_report_map_handle, read_report_map_cb, NULL);
    ESP_LOGI(TAG, "Joystick ready");
    return;
  }

  uint16_t cccd = s_chrs[s_cccd_en_idx].cccd_handle;
  if (cccd == 0) {
    cccd = s_chrs[s_cccd_en_idx].val_handle + 1;
    ESP_LOGI(TAG, "CCCD not found, trying val+1=%d", cccd);
  }

  static const uint8_t notify_on[2] = {0x01, 0x00};
  ble_gattc_write_flat(s_conn_handle, cccd,
                        notify_on, sizeof(notify_on), write_cccd_cb, NULL);
}

/* ── Descriptor discovery ────────────────────────────────────────── */
static int dsc_disc_cb(uint16_t ch, const struct ble_gatt_error *err,
                        uint16_t chr_def_h, const struct ble_gatt_dsc *dsc, void *arg)
{
  if (err->status == BLE_HS_EDONE) {
    s_dsc_chr_idx++;
    start_dsc_discovery();
    return 0;
  }
  if (err->status != 0) {
    ESP_LOGW(TAG, "dsc disc err %d", err->status);
    s_dsc_chr_idx++;
    start_dsc_discovery();
    return err->status;
  }
  if (!dsc) return 0;

  uint16_t uuid16 = (dsc->uuid.u.type == BLE_UUID_TYPE_16)
                        ? ((const ble_uuid16_t *)&dsc->uuid)->value : 0;
  ESP_LOGI(TAG, "  dsc handle=%d uuid=0x%04x", dsc->handle, uuid16);

  if (ble_uuid_cmp(&dsc->uuid.u, &s_cccd_uuid.u) == 0) {
    s_chrs[s_dsc_chr_idx].cccd_handle = dsc->handle;
    ESP_LOGI(TAG, "  CCCD found: val=%d cccd=%d",
             s_chrs[s_dsc_chr_idx].val_handle, dsc->handle);
  } else if (uuid16 == 0x2a4c) {
    s_hid_cp_handle = dsc->handle;
    ESP_LOGI(TAG, "  HID Control Point: handle=%d", dsc->handle);
  } else if (uuid16 == 0x2a4b) {
    s_report_map_handle = dsc->handle;
    ESP_LOGI(TAG, "  Report Map: handle=%d", dsc->handle);
  }
  return 0;
}

static void start_dsc_discovery(void)
{
  if (s_dsc_chr_idx >= s_chr_count) {
    s_cccd_en_idx = 0;
    enable_next_cccd();
    return;
  }
  uint16_t start = s_chrs[s_dsc_chr_idx].val_handle + 1;
  uint16_t end   = s_chrs[s_dsc_chr_idx].end_handle;
  if (start > end) {
    s_dsc_chr_idx++;
    start_dsc_discovery();
    return;
  }
  ble_gattc_disc_all_dscs(s_conn_handle, start, end, dsc_disc_cb, NULL);
}

/* ── Characteristic discovery ────────────────────────────────────── */
static int chr_disc_cb(uint16_t ch, const struct ble_gatt_error *err,
                        const struct ble_gatt_chr *chr, void *arg)
{
  if (err->status == BLE_HS_EDONE) {
    for (int i = 0; i < s_chr_count; i++) {
      s_chrs[i].end_handle = (i + 1 < s_chr_count)
                                 ? (s_chrs[i + 1].def_handle - 1)
                                 : s_hid_end;
    }
    ESP_LOGI(TAG, "Chr disc done, %d notify chrs", s_chr_count);
    s_dsc_chr_idx = 0;
    start_dsc_discovery();
    return 0;
  }
  if (err->status != 0) return err->status;
  if (!chr) return 0;

  if ((chr->properties & BLE_GATT_CHR_PROP_NOTIFY) && s_chr_count < MAX_NOTIFY_CHRS) {
    s_chrs[s_chr_count].def_handle  = chr->def_handle;
    s_chrs[s_chr_count].val_handle  = chr->val_handle;
    s_chrs[s_chr_count].cccd_handle = 0;
    s_chrs[s_chr_count].end_handle  = 0;
    s_chr_count++;
    ESP_LOGI(TAG, "Notify chr: def=%d val=%d", chr->def_handle, chr->val_handle);
  }
  return 0;
}

/* ── Service discovery ───────────────────────────────────────────── */
static int svc_disc_cb(uint16_t ch, const struct ble_gatt_error *err,
                        const struct ble_gatt_svc *svc, void *arg)
{
  if (err->status == BLE_HS_EDONE) {
    if (!s_hid_start) {
      ESP_LOGW(TAG, "HID service not found, disconnecting");
      ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
      return 0;
    }
    s_chr_count = 0;
    return ble_gattc_disc_all_chrs(s_conn_handle, s_hid_start, s_hid_end,
                                    chr_disc_cb, NULL);
  }
  if (err->status != 0) return err->status;
  if (!svc) return 0;

  ESP_LOGI(TAG, "HID svc handles %d-%d", svc->start_handle, svc->end_handle);
  s_hid_start = svc->start_handle;
  s_hid_end   = svc->end_handle;
  return 0;
}

/* ── GAP event handler ───────────────────────────────────────────── */
static int gap_event_cb(struct ble_gap_event *ev, void *arg)
{
  switch (ev->type) {

  case BLE_GAP_EVENT_DISC:
    if (name_matches(ev->disc.data, ev->disc.length_data)) {
      ESP_LOGI(TAG, "Found \"%s\"  RSSI=%d", JOY_DEVICE_NAME, ev->disc.rssi);
      ble_gap_disc_cancel();
      struct ble_gap_conn_params cp = {
        .scan_itvl = 0x0010,
        .scan_window = 0x0010,
        .itvl_min  = BLE_GAP_INITIAL_CONN_ITVL_MIN,
        .itvl_max  = BLE_GAP_INITIAL_CONN_ITVL_MAX,
        .latency   = BLE_GAP_INITIAL_CONN_LATENCY,
        .supervision_timeout = BLE_GAP_INITIAL_SUPERVISION_TIMEOUT,
        .min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN,
        .max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN,
      };
      int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &ev->disc.addr,
                                10000, &cp, gap_event_cb, NULL);
      if (rc != 0) {
        ESP_LOGE(TAG, "connect err %d", rc);
        xTimerStart(s_rescan_timer, 0);
      }
    }
    break;

  case BLE_GAP_EVENT_CONNECT:
    if (ev->connect.status == 0) {
      ESP_LOGI(TAG, "Connected handle=%d", ev->connect.conn_handle);
      s_conn_handle = ev->connect.conn_handle;
      s_hid_start = s_hid_end = s_hid_cp_handle = s_report_map_handle = 0;
    } else {
      ESP_LOGW(TAG, "Connect failed status=%d", ev->connect.status);
      s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
      xTimerStart(s_rescan_timer, 0);
    }
    break;

  case BLE_GAP_EVENT_LINK_ESTAB:
    if (ev->link_estab.status == 0 &&
        ev->link_estab.conn_handle == s_conn_handle) {
      struct ble_gap_conn_desc desc;
      if (ble_gap_conn_find(s_conn_handle, &desc) == 0)
        ESP_LOGI(TAG, "LINK_ESTAB encrypted=%d authenticated=%d bonded=%d",
                 desc.sec_state.encrypted,
                 desc.sec_state.authenticated,
                 desc.sec_state.bonded);
      int rc = ble_gap_security_initiate(s_conn_handle);
      if (rc != 0) {
        ESP_LOGW(TAG, "security_initiate err %d, direct discovery", rc);
        ble_gattc_disc_svc_by_uuid(s_conn_handle, &s_hid_svc_uuid.u,
                                     svc_disc_cb, NULL);
      } else {
        ESP_LOGI(TAG, "Pairing initiated, waiting for ENC_CHANGE");
      }
    }
    break;

  case BLE_GAP_EVENT_ENC_CHANGE:
    ESP_LOGI(TAG, "ENC_CHANGE status=%d", ev->enc_change.status);
    if (ev->enc_change.conn_handle == s_conn_handle)
      ble_gattc_disc_svc_by_uuid(s_conn_handle, &s_hid_svc_uuid.u,
                                   svc_disc_cb, NULL);
    break;

  case BLE_GAP_EVENT_REPEAT_PAIRING:
    ESP_LOGI(TAG, "Repeat pairing, retrying");
    return BLE_GAP_REPEAT_PAIRING_RETRY;

  case BLE_GAP_EVENT_DISCONNECT:
    ESP_LOGI(TAG, "Disconnected reason=%d", ev->disconnect.reason);
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    xTimerStart(s_rescan_timer, 0);
    break;

  case BLE_GAP_EVENT_NOTIFY_RX:
    if (!ev->notify_rx.indication) {
      struct os_mbuf *om = ev->notify_rx.om;
      uint16_t ah = ev->notify_rx.attr_handle;
      ESP_LOGI(TAG, "RX attr=%d len=%d %02x %02x", ah, om->om_len,
               om->om_len > 0 ? om->om_data[0] : 0,
               om->om_len > 1 ? om->om_data[1] : 0);
      parse_report(ah, om->om_data, om->om_len);
    }
    break;

  case BLE_GAP_EVENT_DISC_COMPLETE:
    ESP_LOGD(TAG, "Scan complete reason=%d", ev->disc_complete.reason);
    break;

  default:
    break;
  }
  return 0;
}

/* ── Startup task ────────────────────────────────────────────────── */
static void joy_start_task(void *arg)
{
  while (!ble_hs_synced())
    vTaskDelay(pdMS_TO_TICKS(100));
  start_scan();
  vTaskDelete(NULL);
}

/* ── Public init ─────────────────────────────────────────────────── */
esp_err_t ble_joystick_init(void)
{
  esp_log_level_set("JOY", ESP_LOG_DEBUG);

  s_rescan_timer = xTimerCreate("joy_rescan", pdMS_TO_TICKS(JOY_RESCAN_MS),
                                 pdFALSE, NULL, rescan_timer_cb);
  if (!s_rescan_timer) return ESP_ERR_NO_MEM;

  const esp_timer_create_args_t right_ta = { .callback = right_timer_cb, .name = "joy_right" };
  const esp_timer_create_args_t left_ta  = { .callback = left_timer_cb,  .name = "joy_left"  };
  const esp_timer_create_args_t up_ta    = { .callback = up_timer_cb,    .name = "joy_up"    };
  esp_timer_create(&right_ta, &s_right_timer);
  esp_timer_create(&left_ta,  &s_left_timer);
  esp_timer_create(&up_ta,    &s_up_timer);

  ESP_LOGI(TAG, "BLE joystick init  device=\"%s\"", JOY_DEVICE_NAME);

  BaseType_t ok = xTaskCreate(joy_start_task, "joy_start", 2048, NULL, 3, NULL);
  return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}
