/**
 * @file serial_cmd.h
 * @brief UART serial command interface for AP keypad emulation
 *
 * Listens on UART0 (console) for single-character commands:
 *
 *   'a'  – engage AUTO mode
 *   'm'  – cycle AP mode (AUTO → WIND → NAV → STANDBY → …)
 *   '+'  – heading +1°
 *   '-'  – heading -1°
 *   't'  – heading +10°
 *   'T'  – heading -10°
 *   '?'  – print current AP state
 *   'h'  – print help
 */
#pragma once

#include "esp_err.h"

/**
 * Start the serial command task.
 * Must be called after ap_init().
 */
esp_err_t serial_cmd_start(void);
