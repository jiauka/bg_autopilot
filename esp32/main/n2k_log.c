/**
 * @file n2k_log.c
 * @brief Actisense-style NMEA 2000 bus logger implementation.
 */

#include "n2k_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdatomic.h>

static atomic_bool s_enabled = ATOMIC_VAR_INIT(false);

void n2k_log_enable(bool on)
{
  atomic_store(&s_enabled, on);
  printf("\r\n[N2K LOG %s]\r\n", on ? "ON" : "OFF");
}

bool n2k_log_enabled(void)
{
  return atomic_load(&s_enabled);
}

void n2k_log_frame(uint8_t priority, uint32_t pgn, uint8_t src, uint8_t dst, const uint8_t* data, uint8_t len)
{
  if (!atomic_load(&s_enabled))
    return;

  /* Timestamp: uptime formatted as HH:MM:SS.mmm */
  uint32_t ms = (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
  unsigned hh = ms / 3600000u;
  unsigned mm = (ms % 3600000u) / 60000u;
  unsigned ss = (ms % 60000u) / 1000u;
  unsigned mmm = ms % 1000u;

  /* canboat / Actisense ASCII raw format:
   * HH:MM:SS.mmm,priority,pgn,src,dst,len,b0,b1,...  */
  printf("%02u:%02u:%02u.%03u,%u,%lu,%u,%u,%u", hh, mm, ss, mmm, (unsigned)priority, (unsigned long)pgn, (unsigned)src, (unsigned)dst, (unsigned)len);
  for (uint8_t i = 0; i < len; i++)
    printf(",%02x", data[i]);
  printf("\r\n");
}
