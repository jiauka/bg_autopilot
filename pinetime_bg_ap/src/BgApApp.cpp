#include "BgApApp.h"
#include "displayapp/LittleVgl.h"
#include "displayapp/screens/Symbols.h"
#include <cstdio>

namespace Pinetime {
namespace Applications {
namespace Screens {

// ── Colour palette ────────────────────────────────────────────────────
static constexpr lv_color_t C_BG    = LV_COLOR_MAKE(0x00, 0x00, 0x00);
static constexpr lv_color_t C_WHITE = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);
static constexpr lv_color_t C_GREEN = LV_COLOR_MAKE(0x00, 0xE6, 0x76);
static constexpr lv_color_t C_RED   = LV_COLOR_MAKE(0xE5, 0x00, 0x00);
static constexpr lv_color_t C_AMBER = LV_COLOR_MAKE(0xFF, 0xD7, 0x40);
static constexpr lv_color_t C_BLUE  = LV_COLOR_MAKE(0x40, 0xC4, 0xFF);
static constexpr lv_color_t C_GREY  = LV_COLOR_MAKE(0x60, 0x60, 0x60);

lv_design_cb_t BgApApp::sOldDesign = nullptr;

// ── Constructor / Destructor ──────────────────────────────────────────

BgApApp::BgApApp(Components::BleApClient& ble)
  : Screen(), mBle(ble) {

  mBle.SetObserver(this);
  CreateWidgets();
  taskRefresh = lv_task_create(Screen::RefreshTaskCallback, 500, LV_TASK_PRIO_MID, this);
}

BgApApp::~BgApApp() {
  lv_task_del(taskRefresh);
  mBle.SetObserver(nullptr);
  lv_obj_clean(lv_scr_act());
}

// ── Helpers ───────────────────────────────────────────────────────────

static lv_obj_t* MakeQuadBtn(lv_obj_t* parent, const char* label,
                              lv_color_t bg, lv_color_t fg,
                              const lv_font_t* font, lv_event_cb_t cb,
                              int x, int y) {
  lv_obj_t* btn = lv_btn_create(parent, nullptr);
  lv_obj_set_style_local_bg_color(btn, LV_BTN_PART_MAIN, LV_STATE_DEFAULT, bg);
  lv_obj_set_style_local_radius(btn, LV_BTN_PART_MAIN, LV_STATE_DEFAULT, 0);
  lv_obj_set_size(btn, 120, 120);
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_event_cb(btn, cb);

  lv_obj_t* lbl = lv_label_create(btn, nullptr);
  lv_label_set_text(lbl, label);
  lv_obj_set_style_local_text_font(lbl, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, font);
  lv_obj_set_style_local_text_color(lbl, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, fg);
  lv_obj_align(lbl, btn, LV_ALIGN_CENTER, 0, 0);
  return btn;
}

static lv_obj_t* BtnLbl(lv_obj_t* btn) {
  return lv_obj_get_child(btn, nullptr);
}

static void SetBtnLabel(lv_obj_t* btn, const char* text, lv_color_t color) {
  lv_obj_t* lbl = BtnLbl(btn);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_local_text_color(lbl, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, color);
}

// ── Widget creation ───────────────────────────────────────────────────

void BgApApp::CreateWidgets() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_user_data(scr, this);
  lv_obj_set_style_local_bg_color(scr, LV_OBJ_PART_MAIN, LV_STATE_DEFAULT, C_BG);

  // Four full-quadrant buttons (120×120 each, covering the 240×240 screen)
  mBtnUPL = MakeQuadBtn(scr, "AUTO", C_BG, C_GREEN, &jetbrains_mono_42, BtnCb,   0,   0);
  mBtnUPR = MakeQuadBtn(scr, "WIND", C_BG, C_AMBER, &jetbrains_mono_42, BtnCb, 120,   0);
  mBtnBTL = MakeQuadBtn(scr, Symbols::power, C_RED, C_WHITE, &lv_font_sys_48, BtnCb,   0, 120);
  mBtnBTR = MakeQuadBtn(scr, "NAV",  C_BG, C_BLUE,  &jetbrains_mono_42, BtnCb, 120, 120);

  // Overlay: desired value at top-center (pass-through for touch)
  mLblUpText = lv_label_create(scr, nullptr);
  lv_label_set_text(mLblUpText, "---");
  lv_label_set_align(mLblUpText, LV_LABEL_ALIGN_CENTER);
  lv_obj_set_style_local_text_font(mLblUpText, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, &jetbrains_mono_42);
  lv_obj_set_style_local_text_color(mLblUpText, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, C_GREEN);
  lv_obj_align(mLblUpText, scr, LV_ALIGN_IN_TOP_MID, 0, 6);
  lv_obj_set_click(mLblUpText, false);

  // Overlay: actual value at the horizontal mid-line (pass-through for touch)
  mLblBtText = lv_label_create(scr, nullptr);
  lv_label_set_text(mLblBtText, "---");
  lv_label_set_align(mLblBtText, LV_LABEL_ALIGN_CENTER);
  lv_obj_set_style_local_text_font(mLblBtText, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, &jetbrains_mono_42);
  lv_obj_set_style_local_text_color(mLblBtText, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, C_GREEN);
  lv_obj_align(mLblBtText, scr, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_click(mLblBtText, false);
}

// ── Display update ────────────────────────────────────────────────────

void BgApApp::OnBgApStatus(const Components::BgApStatus& status) {
  mStatus      = status;
  mConnected   = true;
  mNeedsUpdate = true;
}

void BgApApp::OnBgApConnected(bool connected) {
  mConnected   = connected;
  mNeedsUpdate = true;
}

void BgApApp::Refresh() {
  if (!mNeedsUpdate) return;
  mNeedsUpdate = false;
  UpdateDisplay();
}

void BgApApp::UpdateDisplay() {
  Components::BgApMode mode = mConnected ? mStatus.mode : Components::BgApMode::Unknown;
  bool isStby = (mode == Components::BgApMode::Standby ||
                 mode == Components::BgApMode::Unknown);

  // ── UPL / UPR / BTR labels ────────────────────────────────────────
  if (isStby) {
    SetBtnLabel(mBtnUPL, "AUTO", C_GREEN);
    SetBtnLabel(mBtnUPR, "WIND", C_AMBER);
    SetBtnLabel(mBtnBTR, "NAV",  C_BLUE);
  } else {
    SetBtnLabel(mBtnUPL, mStepLarge ? "-10" : "-1",   C_RED);
    SetBtnLabel(mBtnUPR, mStepLarge ? "+10" : "+1",   C_GREEN);
    SetBtnLabel(mBtnBTR, "1/10", mStepLarge ? C_AMBER : C_GREY);
  }

  // ── Overlay text values ───────────────────────────────────────────
  if (!mConnected) {
    lv_label_set_text(mLblUpText, "---");
    lv_label_set_text(mLblBtText, "");
    return;
  }
  if (isStby) {
    lv_label_set_text(mLblUpText, "");
    lv_label_set_text(mLblBtText, "");
    return;
  }

  char buf[8];
  snprintf(buf, sizeof(buf), "%.0f", mStatus.apHeading);
  lv_label_set_text(mLblUpText, buf);

  snprintf(buf, sizeof(buf), "%.0f", mStatus.vesselHeading);
  lv_label_set_text(mLblBtText, buf);
}

// ── Button handler ────────────────────────────────────────────────────

void BgApApp::BtnCb(lv_obj_t* btn, lv_event_t ev) {
  lv_obj_t* scr = lv_obj_get_parent(btn);
  if (!scr) return;
  auto* app = static_cast<BgApApp*>(lv_obj_get_user_data(scr));
  if (!app) return;

  Components::BleApClient& ble = app->mBle;

  // Long-press on STBY → disconnect BLE and quit app
  if (btn == app->mBtnBTL && ev == LV_EVENT_LONG_PRESSED) {
    ble.StopAndDisconnect();
    app->running = false;
    return;
  }

  if (ev != LV_EVENT_CLICKED) return;

  Components::BgApMode mode = app->mStatus.mode;
  bool isStby = !app->mConnected ||
                mode == Components::BgApMode::Standby ||
                mode == Components::BgApMode::Unknown;

  if (btn == app->mBtnBTL) {
    ble.SendCmd(Components::BgApCmd::Standby);
  } else if (btn == app->mBtnUPL) {
    ble.SendCmd(isStby ? Components::BgApCmd::Auto
                       : (app->mStepLarge ? Components::BgApCmd::Minus10
                                          : Components::BgApCmd::Minus1));
  } else if (btn == app->mBtnUPR) {
    ble.SendCmd(isStby ? Components::BgApCmd::Wind
                       : (app->mStepLarge ? Components::BgApCmd::Plus10
                                          : Components::BgApCmd::Plus1));
  } else if (btn == app->mBtnBTR) {
    if (isStby) {
      ble.SendCmd(Components::BgApCmd::Nav);
    } else {
      app->mStepLarge = !app->mStepLarge;
      app->mNeedsUpdate = true;
    }
  }
}

bool BgApApp::OnTouchEvent(TouchEvents /*event*/) {
  return false;
}

} // Screens
} // Applications
} // Pinetime
