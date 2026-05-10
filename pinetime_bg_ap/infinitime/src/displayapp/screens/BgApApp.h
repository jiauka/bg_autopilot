#pragma once

#include "displayapp/screens/Screen.h"
#include "displayapp/apps/Apps.h"
#include "displayapp/Controllers.h"
#include "displayapp/screens/Symbols.h"
#include "components/ble/BleApClient.h"
#include "lvgl/lvgl.h"

namespace Pinetime {
namespace Applications {
namespace Screens {

class BgApApp : public Screen, public Components::IBleApObserver {
public:
  explicit BgApApp(Components::BleApClient& ble);
  ~BgApApp() override;

  bool OnTouchEvent(TouchEvents event) override;
  void Refresh() override;

  // IBleApObserver
  void OnBgApStatus(const Components::BgApStatus& status) override;
  void OnBgApConnected(bool connected) override;

private:
  void CreateWidgets();
  void UpdateDisplay();

  static void BtnCb(lv_obj_t* btn, lv_event_t ev);

  Components::BleApClient& mBle;

  volatile bool mNeedsUpdate = false;
  Components::BgApStatus mStatus    = {};
  bool                   mConnected = false;
  bool                   mStepLarge = false;

  lv_task_t* taskRefresh = nullptr;

  // Corner buttons
  lv_obj_t* mBtnUPL = nullptr;  // top-left
  lv_obj_t* mBtnUPR = nullptr;  // top-right
  lv_obj_t* mBtnBTL = nullptr;  // bottom-left (always STBY)
  lv_obj_t* mBtnBTR = nullptr;  // bottom-right

  // Info text areas
  lv_obj_t* mLblUpText = nullptr;  // upper-middle
  lv_obj_t* mLblBtText = nullptr;  // lower-middle

  // Connecting splash
  lv_obj_t* mImgSplash = nullptr;

  static lv_design_cb_t sOldDesign;
};

} // Screens

// ── AppTraits specialisation ──────────────────────────────────────────
template <>
struct AppTraits<Apps::BgAp> {
  static constexpr Apps        app  = Apps::BgAp;
  static constexpr const char* icon = Screens::Symbols::tachometer;

  static Screens::Screen* Create(AppControllers& /*controllers*/) {
    static Components::BleApClient bleClient;
    static bool initialised = false;
    if (!initialised) {
      bleClient.Init();
      initialised = true;
    }
    bleClient.StartScan();
    return new Screens::BgApApp(bleClient);
  }
};

} // Applications
} // Pinetime
