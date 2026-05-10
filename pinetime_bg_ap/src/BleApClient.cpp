#include "BleApClient.h"
#include "os/os_mbuf.h"

namespace Pinetime {
namespace Components {

// UUIDs stored in NimBLE little-endian byte order
// 12340001-1234-1234-1234-123456789ABC → LE: BC 9A 78 56 34 12 34 12 34 12 34 12 01 00 34 12
const ble_uuid128_t BleApClient::kSvcUuid = {
  .u     = { .type = BLE_UUID_TYPE_128 },
  .value = { 0xBC,0x9A,0x78,0x56,0x34,0x12,0x34,0x12,
             0x34,0x12,0x34,0x12,0x01,0x00,0x34,0x12 }
};
const ble_uuid128_t BleApClient::kCmdUuid = {
  .u     = { .type = BLE_UUID_TYPE_128 },
  .value = { 0xBC,0x9A,0x78,0x56,0x34,0x12,0x34,0x12,
             0x34,0x12,0x34,0x12,0x02,0x00,0x34,0x12 }
};
const ble_uuid128_t BleApClient::kStatusUuid = {
  .u     = { .type = BLE_UUID_TYPE_128 },
  .value = { 0xBC,0x9A,0x78,0x56,0x34,0x12,0x34,0x12,
             0x34,0x12,0x34,0x12,0x03,0x00,0x34,0x12 }
};

// ── Static C callbacks ────────────────────────────────────────────────

int BleApClient::GapCb(struct ble_gap_event* ev, void* arg) {
  return static_cast<BleApClient*>(arg)->OnGapEvent(ev);
}
int BleApClient::SvcDiscCb(uint16_t conn, const struct ble_gatt_error* err,
                            const struct ble_gatt_svc* svc, void* arg) {
  return static_cast<BleApClient*>(arg)->OnSvcDisc(conn, err, svc);
}
int BleApClient::ChrDiscCb(uint16_t conn, const struct ble_gatt_error* err,
                            const struct ble_gatt_chr* chr, void* arg) {
  return static_cast<BleApClient*>(arg)->OnChrDisc(conn, err, chr);
}
int BleApClient::CccdCb(uint16_t conn, const struct ble_gatt_error* err,
                         struct ble_gatt_attr* attr, void* arg) {
  return static_cast<BleApClient*>(arg)->OnCccdWrite(conn, err, attr);
}

// ── Public API ────────────────────────────────────────────────────────

void BleApClient::Init() {
  // Nothing needed; NimBLE is already initialised by InfiniTime's NimbleController
}

void BleApClient::StartScan() {
  mAutoReconnect = true;
  ble_hs_id_infer_auto(0, &mAddrType);

  struct ble_gap_disc_params params = {};
  params.filter_duplicates = 1;
  params.passive           = 0;
  params.itvl              = 0;  // use defaults
  params.window            = 0;

  ble_gap_disc_cancel();  // stop any previous scan
  ble_gap_disc(mAddrType, BLE_HS_FOREVER, &params, GapCb, this);
}

void BleApClient::StopAndDisconnect() {
  mAutoReconnect = false;
  ble_gap_disc_cancel();
  if (mConn != BLE_HS_CONN_HANDLE_NONE)
    ble_gap_terminate(mConn, BLE_ERR_REM_USER_CONN_TERM);
}

void BleApClient::SendCmd(uint8_t cmd) {
  if (!mConnected || mConn == BLE_HS_CONN_HANDLE_NONE || mCmdHandle == 0)
    return;
  ble_gattc_write_no_rsp_flat(mConn, mCmdHandle, &cmd, 1);
}

// ── GAP event handler ─────────────────────────────────────────────────

int BleApClient::OnGapEvent(struct ble_gap_event* ev) {
  switch (ev->type) {

    case BLE_GAP_EVENT_DISC: {
      struct ble_hs_adv_fields fields;
      if (ble_hs_adv_parse_fields(&fields, ev->disc.data,
                                   ev->disc.length_data) != 0)
        break;
      if (fields.name != nullptr && fields.name_len == 5 &&
          memcmp(fields.name, "BG_AP", 5) == 0) {
        ble_gap_disc_cancel();
        DoConnect(ev->disc.addr);
      }
      break;
    }

    case BLE_GAP_EVENT_CONNECT: {
      if (ev->connect.status == 0) {
        mConn = ev->connect.conn_handle;
        DoDiscoverSvc();
      } else {
        // Connect failed — retry scan
        StartScan();
      }
      break;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
      mConnected    = false;
      mCmdHandle    = 0;
      mStatusHandle = 0;
      mConn         = BLE_HS_CONN_HANDLE_NONE;
      if (mObserver) mObserver->OnBgApConnected(false);
      if (mAutoReconnect) StartScan();
      break;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
      if (ev->notify_rx.attr_handle == mStatusHandle) {
        uint16_t len = OS_MBUF_PKTLEN(ev->notify_rx.om);
        uint8_t  buf[12];
        os_mbuf_copydata(ev->notify_rx.om, 0,
                         len < sizeof(buf) ? len : sizeof(buf), buf);
        ParseStatus(buf, len);
      }
      break;
    }

    default:
      break;
  }
  return 0;
}

// ── GATT discovery chain ──────────────────────────────────────────────

void BleApClient::DoConnect(const ble_addr_t& addr) {
  int rc = ble_gap_connect(mAddrType, &addr, 5000, nullptr, GapCb, this);
  if (rc != 0) {
    // Connect initiation failed — fall back to scanning
    StartScan();
  }
}

void BleApClient::DoDiscoverSvc() {
  ble_gattc_disc_svc_by_uuid(mConn, &kSvcUuid.u, SvcDiscCb, this);
}

int BleApClient::OnSvcDisc(uint16_t conn, const struct ble_gatt_error* err,
                            const struct ble_gatt_svc* svc) {
  if (err->status == BLE_HS_EDONE) return 0;
  if (err->status != 0)            return err->status;
  if (svc) {
    mSvcEndHandle = svc->end_handle;
    ble_gattc_disc_all_chrs(conn, svc->start_handle, svc->end_handle,
                             ChrDiscCb, this);
  }
  return 0;
}

int BleApClient::OnChrDisc(uint16_t /*conn*/, const struct ble_gatt_error* err,
                             const struct ble_gatt_chr* chr) {
  if (err->status == BLE_HS_EDONE) {
    DoSubscribeNotify();
    return 0;
  }
  if (err->status != 0) return err->status;
  if (!chr)             return 0;

  if (ble_uuid_cmp(&chr->uuid.u, &kCmdUuid.u) == 0)
    mCmdHandle = chr->val_handle;
  else if (ble_uuid_cmp(&chr->uuid.u, &kStatusUuid.u) == 0)
    mStatusHandle = chr->val_handle;

  return 0;
}

void BleApClient::DoSubscribeNotify() {
  if (mStatusHandle == 0) return;
  // CCCD sits at val_handle + 1 (CMD char is write-only, no descriptor)
  uint16_t cccd = mStatusHandle + 1;
  uint16_t val  = htole16(0x0001);  // BLE CCCD: enable notifications
  ble_gattc_write_flat(mConn, cccd, &val, sizeof(val), CccdCb, this);
}

int BleApClient::OnCccdWrite(uint16_t /*conn*/, const struct ble_gatt_error* err,
                              struct ble_gatt_attr* /*attr*/) {
  if (err->status == 0) {
    mConnected = true;
    if (mObserver) mObserver->OnBgApConnected(true);
  }
  return 0;
}

// ── Status packet parser ──────────────────────────────────────────────

void BleApClient::ParseStatus(const uint8_t* d, uint16_t len) {
  if (len < 7) return;

  mStatus.mode = [&] {
    switch (d[0]) {
      case 0: return BgApMode::Standby;
      case 1: return BgApMode::Auto;
      case 2: return BgApMode::Nfu;
      case 3: return BgApMode::Wind;
      case 4: return BgApMode::Nav;
      default: return BgApMode::Unknown;
    }
  }();

  mStatus.vesselHeading = static_cast<float>(
      static_cast<int16_t>(d[1] | (d[2] << 8))) / 10.0f;
  mStatus.apHeading = static_cast<float>(
      static_cast<int16_t>(d[3] | (d[4] << 8))) / 10.0f;
  mStatus.statusValid = d[5] != 0;
  mStatus.n2kAddr     = d[6];

  if (mObserver) mObserver->OnBgApStatus(mStatus);
}

} // Components
} // Pinetime
