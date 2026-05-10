/**
 * @file serial_cmd.c
 * @brief UART serial command processor for AP keypad emulation
 *
 * Uses the IDF UART driver on UART0 at 115200 8N1.
 * Pin numbers are configured via menuconfig (B&G Autopilot → Serial pins).
 * Single-character commands are processed immediately on receipt.
 */

#include "serial_cmd.h"
#include "autopilot.h"
#include "nmea2000.h"
#include "n2k_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char* TAG = "CMD";

#define CMD_UART_PORT UART_NUM_0
#define CMD_UART_BAUD 115200
#define CMD_UART_TXD CONFIG_BG_AP_UART_TX_GPIO
#define CMD_UART_RXD CONFIG_BG_AP_UART_RX_GPIO
#define CMD_BUF_SIZE 256

static void print_help(void)
{
  printf("\r\n"
         "=== B&G/Simrad AP Keypad Emulator ===\r\n"
         "  s    Engage STANDBY (disengage)\r\n"
         "  a    Engage AUTO mode\r\n"
         "  m    Cycle AP mode\r\n"
         "  w    Engage WIND mode (AUTO then MODE)\r\n"
         "  n    Engage NAV mode (AUTO then MODE x2)\r\n"
         "  +    Heading +1 degree (starboard)\r\n"
         "  -    Heading -1 degree (port)\r\n"
         "  t    Heading +10 degrees (starboard)\r\n"
         "  T    Heading -10 degrees (port)\r\n"
         "  l    Toggle N2K bus log (Actisense format)\r\n"
         "  ?    Show current AP state\r\n"
         "  h    Show this help\r\n"
         "=====================================\r\n");
}

static void print_state(void)
{
  ap_state_t* st = ap_get_state();
  const char* mode_str[] = { "STANDBY", "AUTO", "NFU", "WIND", "NAV", "?", "?", "UNKNOWN" };
  ap_mode_t disp_mode = st->mode;
  const char* src = st->ap_status_valid ? "confirmed" : "assumed";

  printf("\r\n--- AP State ---\r\n"
         "  Mode     : %s  [%s]\r\n"
         "  Vessel hdg: %.1f\xc2\xb0\r\n"
         "  AP cmd hdg: %.1f\xc2\xb0\r\n"
         "  AP addr  : 0x%02X\r\n"
         "  My addr  : 0x%02X\r\n"
         "  Status   : %s\r\n"
         "----------------\r\n",
         mode_str[(int)disp_mode < 8 ? (int)disp_mode : 7],
         src,
         st->vessel_heading,
         st->ap_heading,
         st->ap_src_addr,
         n2k_get_src_addr(),
         st->ap_status_valid ? "LIVE" : "NO DATA FROM AP YET");
}

static void process_char(char c)
{
  esp_err_t rc = ESP_OK;

  switch (c) {
    case 's':
    case 'S':
      printf("CMD: STANDBY\r\n");
      rc = ap_standby();
      break;

    case 'a':
    case 'A':
      printf("CMD: AUTO\r\n");
      rc = ap_engage_auto();
      break;

    case 'm':
    case 'M':
      printf("CMD: MODE cycle\r\n");
      rc = ap_cycle_mode();
      break;

    case 'w':
      printf("CMD: WIND mode\r\n");
      rc = ap_engage_wind();
      break;

    case 'n':
      printf("CMD: NAV mode\r\n");
      rc = ap_engage_nav();
      break;

    case '+':
      printf("CMD: +1\r\n");
      rc = ap_adjust_heading(+1);
      break;

    case '-':
      printf("CMD: -1\r\n");
      rc = ap_adjust_heading(-1);
      break;

    case 't':
      printf("CMD: +10\r\n");
      rc = ap_adjust_heading(+10);
      break;

    case 'T':
      printf("CMD: -10\r\n");
      rc = ap_adjust_heading(-10);
      break;

    case 'l':
    case 'L':
      n2k_log_enable(!n2k_log_enabled());
      return;

    case '?':
      print_state();
      return;

    case 'h':
    case 'H':
      print_help();
      return;

    case '\r':
    case '\n':
    case ' ':
      return; /* ignore whitespace */

    default:
      printf("Unknown command '%c' – press 'h' for help\r\n", c);
      return;
  }

  if (rc != ESP_OK) {
    printf("  -> ERROR: %s\r\n", esp_err_to_name(rc));
  } else {
    printf("  -> OK\r\n");
  }
}

static void serial_cmd_task(void* arg)
{
  uint8_t buf[CMD_BUF_SIZE];
  int len;

  printf("\r\nB&G AP Keypad emulator ready. Press 'h' for help.\r\n");

  while (1) {
    len = uart_read_bytes(CMD_UART_PORT, buf, sizeof(buf) - 1, pdMS_TO_TICKS(50));
    for (int i = 0; i < len; i++) {
      process_char((char)buf[i]);
    }
  }
}

esp_err_t serial_cmd_start(void)
{
  uart_config_t uart_cfg = {
    .baud_rate = CMD_UART_BAUD,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_DEFAULT,
  };

  ESP_RETURN_ON_ERROR(uart_param_config(CMD_UART_PORT, &uart_cfg), TAG, "uart_param_config failed");

  ESP_RETURN_ON_ERROR(uart_set_pin(CMD_UART_PORT, CMD_UART_TXD, CMD_UART_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart_set_pin failed");

  ESP_RETURN_ON_ERROR(uart_driver_install(CMD_UART_PORT, CMD_BUF_SIZE * 2, 0, 0, NULL, 0), TAG, "uart_driver_install failed");

  BaseType_t ok = xTaskCreate(serial_cmd_task, "serial_cmd", 3072, NULL, 4, NULL);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create serial_cmd task");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Serial command interface started on UART%d @ %d baud", CMD_UART_PORT, CMD_UART_BAUD);
  return ESP_OK;
}
