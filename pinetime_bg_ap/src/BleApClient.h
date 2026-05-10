#pragma once

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
// NimBLE defines min/max as 2-arg macros; undefine before any C++ STL headers
#ifdef min
#  undef min
#endif
#ifdef max
#  undef max
#endif
#include <cstdint>
#include <cstring>

namespace Pinetime {
namespace Components {

enum class BgApMode : uint8_t {
  Standby = 0,
  Auto    = 1,
  Nfu     = 2,
  Wind    = 3,
  Nav     = 4,
  Unknown = 0xFF
};

struct BgApStatus {
  BgApMode mode          = BgApMode::Unknown;
  float    vesselHeading = 0.0f;
  float    apHeading     = 0.0f;
  bool     statusValid   = false;
  uint8_t  n2kAddr       = 0xFF;
};

namespace BgApCmd {
  constexpr uint8_t Standby  = 0x01;
  constexpr uint8_t Auto     = 0x02;
  constexpr uint8_t Wind     = 0x03;
  constexpr uint8_t Nav      = 0x04;
  constexpr uint8_t Plus1    = 0x05;
  constexpr uint8_t Minus1   = 0x06;
  constexpr uint8_t Plus10   = 0x07;
  constexpr uint8_t Minus10  = 0x08;
}

// Observer interface — implemented by the UI screen
class IBleApObserver {
public:
  virtual void OnBgApStatus(const BgApStatus& status) = 0;
  virtual void OnBgApConnected(bool connected)        = 0;
protected:
  ~IBleApObserver() = default;
};

class BleApClient {
public:
  void Init();
  void StartScan();
  void StopAndDisconnect();
  void SendCmd(uint8_t cmd);

  void SetObserver(IBleApObserver* obs) { mObserver = obs; }

  bool              IsConnected() const { return mConnected; }
  const BgApStatus& Status()      const { return mStatus; }

  // Called by static C callbacks — public for C linkage
  int OnGapEvent(struct ble_gap_event* ev);
  int OnSvcDisc(uint16_t conn, const struct ble_gatt_error* err, const struct ble_gatt_svc* svc);
  int OnChrDisc(uint16_t conn, const struct ble_gatt_error* err, const struct ble_gatt_chr* chr);
  int OnCccdWrite(uint16_t conn, const struct ble_gatt_error* err, struct ble_gatt_attr* attr);

private:
  void DoConnect(const ble_addr_t& addr);
  void DoDiscoverSvc();
  void DoSubscribeNotify();
  void ParseStatus(const uint8_t* d, uint16_t len);

  static int GapCb(struct ble_gap_event* ev, void* arg);
  static int SvcDiscCb(uint16_t c, const struct ble_gatt_error* e, const struct ble_gatt_svc* s, void* arg);
  static int ChrDiscCb(uint16_t c, const struct ble_gatt_error* e, const struct ble_gatt_chr* chr, void* arg);
  static int CccdCb(uint16_t c, const struct ble_gatt_error* e, struct ble_gatt_attr* a, void* arg);

  bool              mAutoReconnect = true;
  bool              mConnected    = false;
  uint16_t          mConn         = BLE_HS_CONN_HANDLE_NONE;
  uint16_t          mCmdHandle    = 0;
  uint16_t          mStatusHandle = 0;
  uint16_t          mSvcEndHandle = 0;
  uint8_t           mAddrType     = BLE_OWN_ADDR_RANDOM;
  IBleApObserver*   mObserver     = nullptr;
  BgApStatus        mStatus       = {};

  // 12340001-1234-1234-1234-123456789ABC (NimBLE LE byte order)
  static const ble_uuid128_t kSvcUuid;
  static const ble_uuid128_t kCmdUuid;
  static const ble_uuid128_t kStatusUuid;
};

} // Components
} // Pinetime
