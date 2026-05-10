/**
 * @file led_status.c
 * @brief WS2812 RGB LED N2K network status indicator.
 *
 * Uses the IDF 5.x RMT TX driver with a local WS2812 encoder.
 * WS2812B byte order on the wire: Green → Red → Blue (GRB).
 */

#include "led_status.h"
#include "led_strip_encoder.h"

#include "driver/rmt_tx.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>

static const char* TAG = "LED";

/* ── Tunables ───────────────────────────────────────────────────────── */
#define RMT_RESOLUTION_HZ 10000000u /* 10 MHz – WS2812 needs ≥5 MHz  */
#define NET_TIMEOUT_MS 3000u        /* no message for this long → red */
#define BLINK_PERIOD_TICKS 10u      /* ×100 ms = 1 s period           */

/* Brightness 0-255.  WS2812 at 255 is very bright; 20 is clearly visible. */
#define LED_BRIGHTNESS CONFIG_BG_AP_LED_BRIGHTNESS

/* ── State shared between rx_task and led_task ──────────────────────── */
static atomic_uint_fast32_t s_last_rx_ms = ATOMIC_VAR_INIT(0);
static atomic_uchar s_green_state = ATOMIC_VAR_INIT(0);

/* ── RMT handles ────────────────────────────────────────────────────── */
static rmt_channel_handle_t s_rmt_chan;
static rmt_encoder_handle_t s_encoder;

static const rmt_transmit_config_t s_tx_cfg = { .loop_count = 0 };

/* ── Pixel buffer: WS2812B wire order is GRB ────────────────────────── */
static uint8_t s_pixel[3]; /* [0]=Green [1]=Red [2]=Blue */

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
  s_pixel[0] = g;
  s_pixel[1] = r;
  s_pixel[2] = b;
  /* Wait for previous transmission to finish before touching the buffer */
  rmt_tx_wait_all_done(s_rmt_chan, pdMS_TO_TICKS(10));
  rmt_transmit(s_rmt_chan, s_encoder, s_pixel, sizeof(s_pixel), &s_tx_cfg);
}

/* ── LED task ───────────────────────────────────────────────────────── */
static void led_task(void* arg)
{
  uint32_t tick = 0; /* increments every 100 ms */

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(100));
    tick++;

    uint32_t now_ms = (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
    uint32_t last_rx = (uint32_t)atomic_load_explicit(&s_last_rx_ms, memory_order_relaxed);
    bool net_ok = (last_rx != 0) && ((now_ms - last_rx) < NET_TIMEOUT_MS);

    if (!net_ok) {
      /* NO_NETWORK: slow 1 Hz red blink (500 ms on / 500 ms off) */
      if ((tick % BLINK_PERIOD_TICKS) < (BLINK_PERIOD_TICKS / 2)) {
        led_set(LED_BRIGHTNESS, 0, 0); /* red */
      } else {
        led_set(0, 0, 0); /* off */
      }
    } else {
      /* NETWORK_OK: green mirrors rx-toggle state */
      uint8_t g = atomic_load_explicit(&s_green_state, memory_order_relaxed);
      if (g & 1u) {
        led_set(0, LED_BRIGHTNESS, 0); /* green */
      } else {
        led_set(0, 0, 0); /* off */
      }
    }
  }
}

/* ── Public API ─────────────────────────────────────────────────────── */

void led_status_rx_activity(void)
{
  uint32_t ms = (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
  /* Ensure sentinel 0 is never stored as a real timestamp */
  if (ms == 0)
    ms = 1;
  atomic_store_explicit(&s_last_rx_ms, ms, memory_order_relaxed);
  atomic_fetch_xor_explicit(&s_green_state, 1u, memory_order_relaxed);
}

esp_err_t led_status_init(void)
{
  rmt_tx_channel_config_t tx_cfg = {
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .gpio_num = CONFIG_BG_AP_LED_GPIO,
    .mem_block_symbols = 64,
    .resolution_hz = RMT_RESOLUTION_HZ,
    .trans_queue_depth = 4,
  };
  ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_cfg, &s_rmt_chan), TAG, "rmt_new_tx_channel");

  led_strip_encoder_config_t enc_cfg = {
    .resolution = RMT_RESOLUTION_HZ,
  };
  ESP_RETURN_ON_ERROR(rmt_new_led_strip_encoder(&enc_cfg, &s_encoder), TAG, "rmt_new_led_strip_encoder");

  ESP_RETURN_ON_ERROR(rmt_enable(s_rmt_chan), TAG, "rmt_enable");

  /* Start with LED off */
  led_set(0, 0, 0);

  BaseType_t ok = xTaskCreate(led_task, "led_status", 2048, NULL, 3, NULL);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create led_status task");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "LED status on GPIO %d  brightness=%d", CONFIG_BG_AP_LED_GPIO, LED_BRIGHTNESS);
  return ESP_OK;
}
