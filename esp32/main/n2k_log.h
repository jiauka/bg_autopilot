/**
 * @file n2k_log.h
 * @brief On-demand Actisense-style NMEA 2000 bus logger.
 *
 * When enabled every complete N2K message (TX and RX) is printed to the
 * console in the canboat / Actisense ASCII raw format:
 *
 *   HH:MM:SS.mmm,<priority>,<pgn>,<src>,<dst>,<len>,<b0>,<b1>,...
 *
 * Time is uptime-from-boot (no RTC needed).  The format is directly
 * consumable by canboat's `analyzer` tool:
 *
 *   idf.py monitor | grep -E '^[0-9]{2}:' | analyzer -json
 *
 * Toggle via:
 *   - Serial: 'l' key
 *   - BLE:    command byte 0x0A
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /** Enable or disable the bus log. Thread-safe (atomic). */
  void n2k_log_enable(bool on);

  /** Returns current log state. */
  bool n2k_log_enabled(void);

  /**
   * Log one complete N2K message.  Called internally by nmea2000.c for
   * both TX (src == our address) and RX (src == remote node) messages.
   * No-op when logging is disabled.
   */
  void n2k_log_frame(uint8_t priority, uint32_t pgn, uint8_t src, uint8_t dst, const uint8_t* data, uint8_t len);

#ifdef __cplusplus
}
#endif
