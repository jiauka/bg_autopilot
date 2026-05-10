/**
 * @file ble_ap.c
 * @brief BLE GATT server using NimBLE (IDF 5.x built-in)
 *
 * Provides a custom service that a WearOS app can connect to in order to:
 *   - Send AP commands (write to CMD characteristic)
 *   - Receive live AP state (subscribe to STATUS notifications)
 *
 * NimBLE is the default BLE stack in IDF 5.x for ESP32-C3.
 * Enable it in sdkconfig: CONFIG_BT_NIMBLE_ENABLED=y
 */

#include "ble_ap.h"
#include "autopilot.h"
#include "n2k_log.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

/* NimBLE headers */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* Bond store — provides NVS-backed read/write/delete callbacks for SM. */
void ble_store_config_init(void);

static const char* TAG = "BLE";

#define DEVICE_NAME "BG_AP"

/* ── UUIDs ──────────────────────────────────────────────────────────────
 * Using 128-bit custom UUIDs.
 * Base: 12340000-1234-1234-1234-123456789ABC
 */
/* Service */
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x01, 0x00, 0x34, 0x12);

/* CMD characteristic – write-no-response */
static const ble_uuid128_t chr_cmd_uuid = BLE_UUID128_INIT(0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x02, 0x00, 0x34, 0x12);

/* STATUS characteristic – read + notify */
static const ble_uuid128_t chr_status_uuid = BLE_UUID128_INIT(0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x03, 0x00, 0x34, 0x12);

/* ── Internal state ─────────────────────────────────────────────────── */
#define BLE_MAX_CONNECTIONS 2

static uint16_t s_conn_handles[BLE_MAX_CONNECTIONS] = { BLE_HS_CONN_HANDLE_NONE, BLE_HS_CONN_HANDLE_NONE };
static bool s_notify_enabled[BLE_MAX_CONNECTIONS] = { false, false };
static uint16_t s_status_val_handle = 0;
static bool s_nimble_initialized = false;

static int find_conn_slot(uint16_t handle)
{
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++)
    if (s_conn_handles[i] == handle)
      return i;
  return -1;
}

static int find_free_slot(void)
{
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++)
    if (s_conn_handles[i] == BLE_HS_CONN_HANDLE_NONE)
      return i;
  return -1;
}

/* ── Status packet builder ──────────────────────────────────────────── */
#define STATUS_LEN 12

static void build_status_packet(uint8_t pkt[STATUS_LEN])
{
  ap_state_t* st = ap_get_state();

  /* Byte 0: AP mode */
  pkt[0] = (uint8_t)st->mode; /* local commanded state – ap_mode unreliable */

  /* Bytes 1-2: vessel heading × 10 */
  uint16_t vhdg = (uint16_t)(st->vessel_heading * 10.0f);
  pkt[1] = (uint8_t)(vhdg & 0xFF);
  pkt[2] = (uint8_t)(vhdg >> 8);

  /* Bytes 3-4: AP commanded heading × 10 */
  uint16_t ahdg = (uint16_t)(st->ap_heading * 10.0f);
  pkt[3] = (uint8_t)(ahdg & 0xFF);
  pkt[4] = (uint8_t)(ahdg >> 8);

  /* Byte 5: status valid flag */
  pkt[5] = st->ap_status_valid ? 0x01 : 0x00;

  /* Byte 6: AP N2K address */
  pkt[6] = st->ap_src_addr;

  /* Bytes 7-11: reserved */
  pkt[7] = pkt[8] = pkt[9] = pkt[10] = pkt[11] = 0x00;
#if 0
    printf("build_status_packet: ");
    for(int i =0; i< STATUS_LEN; i++) printf("%02x ",pkt[i]);
    printf("\n");
#endif
}

/* ── Command handler ────────────────────────────────────────────────── */
static void handle_ble_command(uint8_t cmd)
{
  ESP_LOGI(TAG, "BLE cmd 0x%02X", cmd);
  switch (cmd) {
    case BLE_CMD_STANDBY:
      ap_standby();
      break;
    case BLE_CMD_AUTO:
      ap_engage_auto();
      break;
    case BLE_CMD_WIND:
      ap_engage_wind();
      break;
    case BLE_CMD_NAV:
      ap_engage_nav();
      break;
    case BLE_CMD_PLUS_1:
      ap_adjust_heading(+1);
      break;
    case BLE_CMD_MINUS_1:
      ap_adjust_heading(-1);
      break;
    case BLE_CMD_PLUS_10:
      ap_adjust_heading(+10);
      break;
    case BLE_CMD_MINUS_10:
      ap_adjust_heading(-10);
      break;
      //    case BLE_CMD_MODE_CYCLE: ap_cycle_mode();                      break;
    case BLE_CMD_LOG_TOGGLE:
      n2k_log_enable(!n2k_log_enabled());
      return;
    default:
      ESP_LOGW(TAG, "Unknown BLE command 0x%02X", cmd);
      break;
  }
  /* Push updated status back to watch */
  ble_ap_notify_status();
}

/* ── GATT characteristic access callbacks ───────────────────────────── */
static int gatt_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg)
{
  const ble_uuid_t* uuid = ctxt->chr->uuid;

  /* CMD characteristic – write */
  if (ble_uuid_cmp(uuid, &chr_cmd_uuid.u) == 0) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
      if (ctxt->om->om_len >= 1) {
        uint8_t cmd = ctxt->om->om_data[0];
        handle_ble_command(cmd);
      }
    }
    return 0;
  }

  /* STATUS characteristic – read */
  if (ble_uuid_cmp(uuid, &chr_status_uuid.u) == 0) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
      uint8_t pkt[STATUS_LEN];
      build_status_packet(pkt);
      int rc = os_mbuf_append(ctxt->om, pkt, STATUS_LEN);
      return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
  }

  return BLE_ATT_ERR_UNLIKELY;
}

/* ── GATT service definition ────────────────────────────────────────── */
static const struct ble_gatt_svc_def s_gatt_svcs[] = {
  {
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = &svc_uuid.u,
    .characteristics =
      (struct ble_gatt_chr_def[]){
        {
          /* CMD: write without response */
          .uuid = &chr_cmd_uuid.u,
          .access_cb = gatt_chr_access,
          .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE,
        },
        {
          /* STATUS: read + notify */
          .uuid = &chr_status_uuid.u,
          .access_cb = gatt_chr_access,
          .val_handle = &s_status_val_handle,
          .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        },
        { 0 } /* terminator */
      },
  },
  { 0 } /* terminator */
};

/* ── Notify helper ──────────────────────────────────────────────────── */
void ble_ap_notify_status(void)
{
  if (s_status_val_handle == 0)
    return;

  uint8_t pkt[STATUS_LEN];
  build_status_packet(pkt);

  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (!s_notify_enabled[i] || s_conn_handles[i] == BLE_HS_CONN_HANDLE_NONE)
      continue;

    struct os_mbuf* om = ble_hs_mbuf_from_flat(NULL, 0);
    if (!om)
      continue;

    int rc = os_mbuf_append(om, pkt, STATUS_LEN);
    if (rc != 0) {
      os_mbuf_free_chain(om);
      continue;
    }

    rc = ble_gatts_notify_custom(s_conn_handles[i], s_status_val_handle, om);
    if (rc != 0 && rc != BLE_HS_ENOTCONN)
      ESP_LOGW(TAG, "Notify[%d] failed: %d", i, rc);
  }
}

static void start_advertising(void);

/* ── GAP event handler ──────────────────────────────────────────────── */
static int gap_event_handler(struct ble_gap_event* event, void* arg)
{
  switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        int slot = find_free_slot();
        if (slot < 0) {
          ESP_LOGW(TAG, "No free slot, rejecting handle=%d", event->connect.conn_handle);
          ble_gap_terminate(event->connect.conn_handle, BLE_ERR_CONN_LIMIT);
          break;
        }
        s_conn_handles[slot] = event->connect.conn_handle;
        s_notify_enabled[slot] = false;
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
          ESP_LOGI(TAG,
                   "Connected[%d] handle=%d MAC=%02X:%02X:%02X:%02X:%02X:%02X",
                   slot,
                   event->connect.conn_handle,
                   desc.peer_id_addr.val[5],
                   desc.peer_id_addr.val[4],
                   desc.peer_id_addr.val[3],
                   desc.peer_id_addr.val[2],
                   desc.peer_id_addr.val[1],
                   desc.peer_id_addr.val[0]);
        } else {
          ESP_LOGI(TAG, "Connected[%d] handle=%d", slot, event->connect.conn_handle);
        }
        start_advertising(); /* keep advertising so a second device can connect */
        ble_ap_notify_status();
      } else {
        ESP_LOGW(TAG, "Connect failed, status=%d", event->connect.status);
        start_advertising();
      }
      break;

    case BLE_GAP_EVENT_DISCONNECT: {
      int slot = find_conn_slot(event->disconnect.conn.conn_handle);
      if (slot >= 0) {
        ESP_LOGI(TAG, "Disconnected[%d] handle=%d reason=%d", slot, event->disconnect.conn.conn_handle, event->disconnect.reason);
        s_conn_handles[slot] = BLE_HS_CONN_HANDLE_NONE;
        s_notify_enabled[slot] = false;
      }
      if (!ble_gap_adv_active())
        start_advertising();
      break;
    }

    case BLE_GAP_EVENT_SUBSCRIBE: {
      if (event->subscribe.attr_handle == s_status_val_handle) {
        int slot = find_conn_slot(event->subscribe.conn_handle);
        if (slot >= 0) {
          s_notify_enabled[slot] = (event->subscribe.cur_notify != 0);
          ESP_LOGI(TAG, "Notifications[%d] %s", slot, s_notify_enabled[slot] ? "ENABLED" : "DISABLED");
          if (s_notify_enabled[slot])
            ble_ap_notify_status();
        }
      }
      break;
    }

    case BLE_GAP_EVENT_MTU:
      ESP_LOGI(TAG, "MTU update  conn=%d mtu=%d", event->mtu.conn_handle, event->mtu.value);
      break;

    default:
      break;
  }
  return 0;
}

/* ── Advertising ────────────────────────────────────────────────────── */
static void start_advertising(void)
{
  struct ble_hs_adv_fields fields = { 0 };

  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.tx_pwr_lvl_is_present = 1;
  fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
  fields.name = (uint8_t*)DEVICE_NAME;
  fields.name_len = strlen(DEVICE_NAME);
  fields.name_is_complete = 1;

  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) {
    ESP_LOGE(TAG, "adv_set_fields error: %d", rc);
    return;
  }

  struct ble_gap_adv_params adv_params = { 0 };
  adv_params.conn_mode = BLE_GAP_CONN_MODE_UND; /* connectable undirected */
  adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN; /* general discoverable   */

  rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, gap_event_handler, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "adv_start error: %d", rc);
    return;
  }
  ESP_LOGI(TAG, "Advertising as \"%s\"", DEVICE_NAME);
}

/* ── NimBLE sync callback ───────────────────────────────────────────── */
static void ble_on_sync(void)
{
  int rc = ble_hs_util_ensure_addr(0);
  assert(rc == 0);
  start_advertising();
}

static void ble_on_reset(int reason)
{
  ESP_LOGE(TAG, "BLE reset, reason=%d", reason);
}

/* ── NimBLE host task ───────────────────────────────────────────────── */
static void nimble_host_task(void* param)
{
  ESP_LOGI(TAG, "NimBLE host task started");
  nimble_port_run(); /* blocks until nimble_port_stop() */
  nimble_port_freertos_deinit();
}

/* ── Public init ────────────────────────────────────────────────────── */
esp_err_t ble_ap_init(void)
{
  /* NimBLE only tolerates one nimble_port_init() for the lifetime of the
   * application.  On subsequent calls (e.g. from a reconnect path that
   * accidentally reached here) just restart advertising and return. */
  if (s_nimble_initialized) {
    if (ble_gap_adv_active())
      ble_gap_adv_stop();
    start_advertising();
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Initialising NimBLE");

  esp_err_t rc = nimble_port_init();
  if (rc != ESP_OK) {
    ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
    return rc;
  }

  /* Register callbacks */
  ble_hs_cfg.sync_cb = ble_on_sync;
  ble_hs_cfg.reset_cb = ble_on_reset;
  /* Just Works pairing (IO cap NO_IO = no passkey).
   * Required for BLE HID peripherals such as the Magicsee R1 joystick. */
  ble_hs_cfg.sm_bonding         = 1;
  ble_hs_cfg.sm_sc              = 0;   /* LE Legacy pairing; R1 doesn't support LESC */
  ble_hs_cfg.sm_io_cap          = BLE_SM_IO_CAP_NO_IO;
  ble_hs_cfg.sm_our_key_dist    = BLE_SM_PAIR_KEY_DIST_ENC;
  ble_hs_cfg.sm_their_key_dist  = BLE_SM_PAIR_KEY_DIST_ENC;
  ble_hs_cfg.store_status_cb    = ble_store_util_status_rr;

  /* Initialise NVS-backed bond store so SM pairing can persist keys. */
  ble_store_config_init();

  /* Register GATT services */
  ble_svc_gap_init();
  ble_svc_gatt_init();

  int ret = ble_gatts_count_cfg(s_gatt_svcs);
  if (ret != 0) {
    ESP_LOGE(TAG, "ble_gatts_count_cfg: %d", ret);
    return ESP_FAIL;
  }
  ret = ble_gatts_add_svcs(s_gatt_svcs);
  if (ret != 0) {
    ESP_LOGE(TAG, "ble_gatts_add_svcs: %d", ret);
    return ESP_FAIL;
  }

  /* Set GAP device name */
  ble_svc_gap_device_name_set(DEVICE_NAME);

  /* Start NimBLE host task */
  nimble_port_freertos_init(nimble_host_task);

  s_nimble_initialized = true;
  ESP_LOGI(TAG, "NimBLE init done");
  return ESP_OK;
}
