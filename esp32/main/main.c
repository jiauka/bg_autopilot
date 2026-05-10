/**
 * @file main.c
 * @brief B&G / Simrad NMEA 2000 Autopilot Keypad Emulator
 *        Targets: ESP32-C3, ESP32-C6, ESP32-S3  |  IDF 5.x  |  No Arduino
 *
 * Both BLE and Serial interfaces can be active simultaneously.
 * Enable/disable each independently via menuconfig:
 *   B&G Autopilot → Enable BLE interface
 *   B&G Autopilot → Enable Serial / UART interface
 *
 * Startup sequence:
 *   1. Initialise NVS flash
 *   2. RGB status LED
 *   3. Initialise N2K / TWAI stack
 *   4. Perform ISO address claim
 *   5. Initialise autopilot module
 *   6. Start enabled command interface(s)
 *   7. Start 1 Hz heartbeat timer
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "nmea2000.h"
#include "autopilot.h"
#include "led_status.h"

#if CONFIG_BG_AP_CMD_BLE
#include "ble_ap.h"
#endif
#if CONFIG_BG_AP_CMD_SERIAL
#include "serial_cmd.h"
#endif
#if CONFIG_BG_AP_FAKE_AP
#include "fake_ap.h"
#endif
#if CONFIG_BG_AP_CMD_JOYSTICK
#include "ble_joystick.h"
#endif

static const char* TAG = "MAIN";

/* ── Periodic N2K timers ────────────────────────────────────────────── */
static void heartbeat_cb(void* arg)
{
  n2k_send_heartbeat();
}
static void keepalive_cb(void* arg)
{
  n2k_send_simnet_keepalive();
}

/* ── Application entry ──────────────────────────────────────────────── */
void app_main(void)
{
  ESP_LOGI(TAG, "====================================");
  ESP_LOGI(TAG, " B&G/Simrad AP Keypad Emulator");
  ESP_LOGI(TAG, " " CONFIG_IDF_TARGET " | IDF 5.x | N2K/TWAI");
#if CONFIG_BG_AP_CMD_BLE && CONFIG_BG_AP_CMD_SERIAL
  ESP_LOGI(TAG, " Interfaces: BLE + Serial");
#elif CONFIG_BG_AP_CMD_BLE
  ESP_LOGI(TAG, " Interface: BLE");
#elif CONFIG_BG_AP_CMD_SERIAL
  ESP_LOGI(TAG, " Interface: Serial");
#else
  ESP_LOGW(TAG, " WARNING: no command interface enabled");
#endif
#if CONFIG_BG_AP_CMD_JOYSTICK
  ESP_LOGI(TAG, " Joystick: BLE HID \"%s\"", CONFIG_BG_AP_JOYSTICK_DEVICE_NAME);
#endif
  ESP_LOGI(TAG, "====================================");

  /* 1. NVS – required by some IDF components */
  esp_err_t rc = nvs_flash_init();
  if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "Erasing NVS flash");
    ESP_ERROR_CHECK(nvs_flash_erase());
    rc = nvs_flash_init();
  }
  ESP_ERROR_CHECK(rc);

  /* 2. RGB status LED (starts red-blinking until N2K traffic arrives) */
  ESP_ERROR_CHECK(led_status_init());

  /* 5. Autopilot module */
  ESP_ERROR_CHECK(ap_init());

  /* 3. Initialise N2K / TWAI stack */
  ESP_ERROR_CHECK(n2k_init());

  /* 4. ISO address claim (blocking, up to ~1 s) */
  ESP_ERROR_CHECK(n2k_address_claim());

  /* 6. Command interface(s) – both may run simultaneously */
#if CONFIG_BG_AP_FAKE_AP
  ESP_ERROR_CHECK(fake_ap_init());
#endif
#if CONFIG_BG_AP_CMD_BLE
  ESP_ERROR_CHECK(ble_ap_init());
#endif
#if CONFIG_BG_AP_CMD_JOYSTICK
  ESP_ERROR_CHECK(ble_joystick_init());
#endif
#if CONFIG_BG_AP_CMD_SERIAL
  ESP_ERROR_CHECK(serial_cmd_start());
#endif

  /* 7. Periodic N2K timers */
  esp_timer_handle_t hb_timer, ka_timer;
  const esp_timer_create_args_t hb_args = {
    .callback = heartbeat_cb,
    .name = "n2k_hb",
    .skip_unhandled_events = true,
  };
  const esp_timer_create_args_t ka_args = {
    .callback = keepalive_cb,
    .name = "n2k_ka",
    .skip_unhandled_events = true,
  };
  ESP_ERROR_CHECK(esp_timer_create(&hb_args, &hb_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(hb_timer, 1000000ULL)); /* 1 Hz */
  ESP_ERROR_CHECK(esp_timer_create(&ka_args, &ka_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(ka_timer, 500000ULL)); /* 2 Hz (PGN 65305) */

  ESP_LOGI(TAG, "System ready. N2K addr=0x%02X", n2k_get_src_addr());

#if CONFIG_BG_AP_CMD_BLE
  /* BLE status push every 500 ms – serial task runs in its own task */
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(500));
    ble_ap_notify_status();
  }
#else
  /* Serial-only: command task handles everything, main just idles */
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
#endif
}
