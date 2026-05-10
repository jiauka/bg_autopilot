/**
 * @file led_status.h
 * @brief WS2812 RGB LED status indicator for N2K network health.
 *
 * GPIO is configured via menuconfig (B&G Autopilot → LED GPIO).
 * Default is GPIO 8 — the onboard RGB LED on ESP32-C3/C6 DevKit boards.
 *
 * Behaviour:
 *   NO_NETWORK  (no message received for >3 s): slow RED blink, 1 Hz
 *   NETWORK_OK  (message received recently)   : GREEN toggles on every
 *                                               received N2K frame
 */
#pragma once
#include "esp_err.h"

/** Initialise RMT channel, WS2812 encoder, and start the LED task. */
esp_err_t led_status_init(void);

/**
 * Signal that a N2K frame has been received.
 * Safe to call from any task (uses atomic operations).
 * Toggles the green LED and resets the network-alive watchdog.
 */
void led_status_rx_activity(void);
