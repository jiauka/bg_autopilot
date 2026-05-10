/**
 * @file nmea2000.cpp
 * @brief NMEA 2000 wrapper using ttlappalainen/NMEA2000 + skarlsson/NMEA2000_twai.
 *
 * Implements the same C API as the previous nmea2000.c so autopilot.c,
 * ble_ap.c, serial_cmd.c and main.c compile unchanged.
 *
 * Threading model:
 *   - n2k_task owns ParseMessages() under a recursive mutex.
 *   - All SendMsg() calls also take the same mutex before dispatching.
 *   - The library may call SendMsg() internally from within ParseMessages()
 *     (e.g. for ISO ACK), so a *recursive* mutex is required.
 *
 * Two virtual devices share the same TWAI bus:
 *   dev 0 – keypad ("Metis AP Keypad",  preferred addr N2K_PREFERRED_ADDR)
 *   dev 1 – AP sim ("AP Simulator",     preferred addr N2K_APSIM_PREFERRED_ADDR)
 */

#include <NMEA2000_esp32_twai.h> /* NMEA2000_esp32_twai – skarlsson TWAI driver */
#include <N2kMessages.h>         /* standard PGN helpers                        */
#include "nmea2000.h"
#include "autopilot.h"
#include "n2k_log.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char* TAG = "N2K";

static double apparentWind=-1;
static uint32_t apparentWindU[5]={0};
static uint16_t apparentWindUCnt=0;
/* AP command handler registered by fake_ap (NULL when fake_ap not used) */
static void (*s_ap_cmd_handler)(uint8_t src, uint8_t dst, const uint8_t* data, uint8_t len) = nullptr;

/* ── NMEA 2000 instance ─────────────────────────────────────────────── */
static NMEA2000_esp32_twai s_nmea2000((gpio_num_t)TWAI_TX_GPIO, (gpio_num_t)TWAI_RX_GPIO);

static SemaphoreHandle_t s_mutex; /* recursive */
static uint8_t boatApMode = 0;    // from PGN65341
static double twaAverage = 0;

/* ── Helpers ─────────────────────────────────────────────────────────── */
static inline void log_msg(const tN2kMsg& msg)
{
  if (n2k_log_enabled())
    n2k_log_frame((uint8_t)msg.Priority, (uint32_t)msg.PGN, msg.Source, msg.Destination, msg.Data, (uint8_t)msg.DataLen);
}

/**
 * @brief Update an approximate rolling average using an N=10 exponential filter.
 *
 * @param[in,out] average     Pointer to the running average value, updated in place.
 * @param[in]     new_sample  New sample to incorporate into the average.
 */
static void approxRollingAverage(const int number,double* average, double new_sample)
{
  *average -= *average / number;
  *average += new_sample / number;
}

/* ── RX message dispatcher ───────────────────────────────────────────── */
static void handle_msg(const tN2kMsg& msg)
{
  log_msg(msg);
  ap_state_t* st = ap_get_state();

  switch (msg.PGN) {
    case PGN_SIMNET_AP_KEEPALIVE:
      if (msg.Data[3] == 0x0a) {
        uint16_t value = (msg.Data[5] << 8 | msg.Data[4]) & 0x0ff0;
        uint8_t ii;                            //                  AC12 capture      / NAC-2 capture           / TP22 capture
        if (value == 0x0000) {
          ap_set_mode(AP_MODE_STANDBY);
          ii = 0x00;   // STBY Mode  41,9f,00,0a,0c,00,80,00 / 41,9f,64,0a,08,00,00,00 / 41,9f,ff,0a,08,00,80,00
        }
        else if (value == 0x0010) {
          ap_set_mode(AP_MODE_AUTO);

          ii = 0x02;   // AUTO Mode  41,9f,00,0a,14,00,80,00 / 41,9f,64,0a,10,00,00,00 / 41,9f,ff,0a,10,00,80,00
        }
        else if (value == 0x0400) {        
          ap_set_mode(AP_MODE_WIND);

          ii = 0x04;   // VANE Mode  41,9f,00,0a,14,04,80,02 / 41,9f,64,0a,00,04,00,04 / 41,9f,ff,0a,10,04,80,06
        }
        else if (value == 0x0050) ii = 0x08;   // TRACK Mode 41,9f,00,0a,54,00,80,00 /                         / 41,9f,ff,0a,54,00,80,00
        else if (value == 0x0110) ii = 0x01;   // NODRIFT M  41,9f,00,0a,14,01,80,00 /
        else                      ii = 0x10;   // NFU
        printf("HandleNMEA2000Msg PGN: %ld APmode %d\n\r", msg.PGN, ii); fflush(stdout);
      }
    break;

    case PGN_SIMNET_AP_MODE: /* 65341  – AP broadcasts mode ~1 Hz */

    {
      // bool ParseN2kPGN65341(const tN2kMsg& N2kMsg, uint16_t &MC,  uint8_t &IC,uint8_t& Mode, uint16_t& Angle)
      uint16_t MC;
      uint8_t IC;
      uint16_t Angle;
      ParseN2kPGN65341(msg, MC, IC, boatApMode, Angle);
      uint16_t a = RadToDeg((float)Angle / 10000.0) + 0.5;
        printf(" 65341 %d %d\n", boatApMode, a);
      if (boatApMode == 3) { // WIND mode
        //ap_set_mode(AP_MODE_WIND);
        ap_on_heading_track(msg.Source,a);
      }
      if (boatApMode == 2) { // AUTO mode
        //ap_set_mode(AP_MODE_AUTO);
      }
#if 0      
    if (msg.DataLen >= 5) {
        n2k_log_frame((uint8_t)msg.Priority, (uint32_t)msg.PGN, msg.Source, msg.Destination, msg.Data, (uint8_t)msg.DataLen);

        uint8_t mode_raw = msg.Data[4];
        uint8_t status1 = (mode_raw != 0x02) ? 0x10 : 0x00;
        printf(" 65341 %d %d %d %d\n", boatApMode, mode_raw,status1, a);
        ap_on_simnet_mode(msg.Source, status1, mode_raw);
      }
#endif      
    }

    break;

    case PGN_HEADING_TRACK: { /* 127237 – commanded heading field 10 */
      tN2kOnOff RudderLimitExceeded;
      tN2kOnOff OffHeadingLimitExceeded;
      tN2kOnOff OffTrackLimitExceeded;
      tN2kOnOff Override;
      tN2kSteeringMode SteeringMode;
      tN2kTurnMode TurnMode;
      tN2kHeadingReference HeadingReference;
      tN2kRudderDirectionOrder CommandedRudderDirection;
      double CommandedRudderAngle;
      double HeadingToSteerCourse;
      double Track;
      double RudderLimit;
      double OffHeadingLimit;
      double RadiusOfTurnOrder;
      double RateOfTurnOrder;
      double OffTrackLimit;
      double VesselHeading;
      ParseN2kPGN127237(msg,
                        RudderLimitExceeded,
                        OffHeadingLimitExceeded,
                        OffTrackLimitExceeded,
                        Override,
                        SteeringMode,
                        TurnMode,
                        HeadingReference,
                        CommandedRudderDirection,
                        CommandedRudderAngle,
                        HeadingToSteerCourse,
                        Track,
                        RudderLimit,
                        OffHeadingLimit,
                        RadiusOfTurnOrder,
                        RateOfTurnOrder,
                        OffTrackLimit,
                        VesselHeading);
      if(boatApMode != 3) {//!WIND mode
        if(SteeringMode != 0) { //AUTO   mode
          printf(" boatApMode %d SteeringMode %d %f\n", boatApMode, SteeringMode, RadToDeg(HeadingToSteerCourse));
//          st->ap_mode=AP_MODE_AUTO;
          ap_on_heading_track(msg.Source, RadToDeg(HeadingToSteerCourse));
        }
      }

      break;
    }

    case PGN_VESSEL_HEADING: { /* 127250 – actual compass heading */
      if (msg.DataLen >= 3) {
        uint16_t raw;
        memcpy(&raw, &msg.Data[1], 2);
        if (raw != 0xFFFF) {
          float hdg = (float)raw * 0.0001f * (180.0f / 3.14159265f);
          if (hdg < 0.0f)
            hdg += 360.0f;
          ap_on_vessel_heading(msg.Source, hdg);
        }
      }
      break;
    }

    case PGN_SIMNET_AP_REPLY: /* 130851 – AP echoes our command back */
      if (msg.DataLen >= 9)
        ap_on_ap_reply(msg.Source, msg.Data[6], msg.Data[8]);
      break;

    case PGN_SIMNET_AP_COMMAND: /* 130850 – keypad → AP command */
      if (s_ap_cmd_handler && msg.DataLen >= 12)
        s_ap_cmd_handler(msg.Source, msg.Destination, msg.Data, (uint8_t)msg.DataLen);
      break;
    case  130306:
      {
    //bool ParseN2kPGN130306(const tN2kMsg& N2kMsg, unsigned char& SID, double& WindSpeed, double& WindAngle, tN2kWindReference& WindReference)
        double windSpeed;
        double windAngle;
        tN2kWindReference ref;
        unsigned char SID;
        ParseN2kPGN130306(msg,SID,windSpeed,windAngle,ref);
         approxRollingAverage(5,&twaAverage, RadToDeg(windAngle));
        
        st->twa=twaAverage;
        printf("WIND %d [130306] %lf\n",__LINE__,twaAverage);
      }
      break;
    default:
      break;
  }
}

/* ── Background task ─────────────────────────────────────────────────── */
static void n2k_task(void* arg)
{
  while (true) {
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    s_nmea2000.ParseMessages();
    xSemaphoreGiveRecursive(s_mutex);
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

/* ── TX helper ───────────────────────────────────────────────────────── */
static esp_err_t send_msg(tN2kMsg& msg, int dev = 0)
{
  xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
  bool ok = s_nmea2000.SendMsg(msg, dev);
  xSemaphoreGiveRecursive(s_mutex);
  log_msg(msg);
  return ok ? ESP_OK : ESP_FAIL;
}

/* ══════════════════════════════════════════════════════════════════════
 * Public C API
 * ══════════════════════════════════════════════════════════════════════ */

extern "C" esp_err_t n2k_init(void)
{
  s_mutex = xSemaphoreCreateRecursiveMutex();
#if CONFIG_BG_AP_FAKE_AP
  s_nmea2000.SetDeviceCount(2);
#endif
  /* dev 0 – keypad */
  s_nmea2000.SetDeviceInformation(N2K_UNIQUE_ID, N2K_DEVICE_FUNCTION, N2K_DEVICE_CLASS, N2K_MANUFACTURER_CODE, N2K_INDUSTRY_CODE, 0);
  s_nmea2000.SetProductInformation("00001234", 100, "Metis AP Keypad", "2026.05.05", "2.0", 1, 2101, 0, 0);
  /* SetMode configures dev 0's preferred address and the shared operating mode */
  s_nmea2000.SetMode(tNMEA2000::N2km_ListenAndNode, N2K_PREFERRED_ADDR);
#if CONFIG_BG_AP_FAKE_AP

  /* dev 1 – AP simulator */
  s_nmea2000.SetDeviceInformation(N2K_APSIM_UNIQUE_ID, N2K_APSIM_DEVICE_FUNCTION, N2K_APSIM_DEVICE_CLASS, N2K_MANUFACTURER_CODE, N2K_APSIM_INDUSTRY_CODE, 1);
  s_nmea2000.SetProductInformation("00000001", 100, "AP Simulator", "2026.05.05", "2.0", 1, 2101, 0, 1);
  /* Set dev 1's preferred source address before Open() */
  s_nmea2000.SetN2kSource(N2K_APSIM_PREFERRED_ADDR, 1);
#endif

  s_nmea2000.SetMsgHandler(handle_msg);
  s_nmea2000.SetHeartbeatInterval(1000); /* 1 Hz heartbeat, all devices */

  /* dev 0 TX PGNs */
  static const unsigned long tx0_pgns[] = { PGN_SIMNET_AP_COMMAND, PGN_SIMNET_AP_KEEPALIVE, PGN_HEARTBEAT, 0 };
  s_nmea2000.ExtendTransmitMessages(tx0_pgns, 0);

  /* dev 1 TX PGNs (AP simulator broadcasts these) */
  static const unsigned long tx1_pgns[] = { PGN_SIMNET_AP_MODE, PGN_SIMNET_AP_STATUS, PGN_SIMNET_AP_STATE, PGN_SIMNET_AP_REPLY, PGN_SIMNET_AP_CTRL,
                                            PGN_HEADING_TRACK,  PGN_VESSEL_HEADING,   PGN_RUDDER,          PGN_HEARTBEAT,       0 };
  s_nmea2000.ExtendTransmitMessages(tx1_pgns, 1);

  /* RX PGNs (shared handler sees all) */
  static const unsigned long rx_pgns[] = { PGN_SIMNET_AP_MODE, PGN_SIMNET_AP_REPLY, PGN_SIMNET_AP_COMMAND, PGN_HEADING_TRACK, PGN_VESSEL_HEADING, 0 };
  s_nmea2000.ExtendReceiveMessages(rx_pgns);

  s_nmea2000.Open();

  BaseType_t ok = xTaskCreate(n2k_task, "n2k", 4096, NULL, 5, NULL);
  if (ok != pdPASS)
    return ESP_FAIL;

  ESP_LOGI(TAG, "N2K init complete (dev0 pref=0x%02X, dev1 pref=0x%02X)", N2K_PREFERRED_ADDR, N2K_APSIM_PREFERRED_ADDR);
  return ESP_OK;
}

extern "C" esp_err_t n2k_address_claim(void)
{
  /* Library performs ISO address claim inside Open()/ParseMessages().
   * Wait long enough for both devices to settle (~260 ms per ISO 11783-5). */
  vTaskDelay(pdMS_TO_TICKS(500));
  ESP_LOGI(TAG, "N2K addrs: dev0=0x%02X  dev1=0x%02X", s_nmea2000.GetN2kSource(0), s_nmea2000.GetN2kSource(1));
  return ESP_OK;
}

extern "C" uint8_t n2k_get_src_addr(void)
{
  return s_nmea2000.GetN2kSource(0);
}

extern "C" uint8_t n2k_get_src_addr_dev(int dev)
{
  return s_nmea2000.GetN2kSource(dev);
}

extern "C" esp_err_t n2k_send_ap_command(uint8_t cmd, uint8_t dir, uint16_t angle, uint8_t ap_addr)
{
  (void)ap_addr; /* broadcast; payload[5] is fixed Simnet field, always 0x0A */

  uint8_t p[12];
  p[0] = 0x41;
  p[1] = 0x9f;
  p[2] = 0x01;
  p[3] = 0xff;
  p[4] = 0xff;
  p[5] = 0x0A;
  p[6] = cmd;
  p[7] = 0x00;

  if (cmd == SIMNET_AP_CMD_COURSE) {
    p[8] = dir;
    p[9] = (uint8_t)(angle & 0xFF);
    p[10] = (uint8_t)(angle >> 8);
    p[11] = 0x00;
  } else if (cmd == SIMNET_AP_CMD_AUTO || cmd == SIMNET_AP_CMD_STANDBY) {
    p[8] = p[9] = p[10] = p[11] = 0xff;
  } else {
    p[8] = p[9] = p[10] = p[11] = 0x00;
  }

  tN2kMsg msg;
  msg.Init(2, PGN_SIMNET_AP_COMMAND, s_nmea2000.GetN2kSource(0), 0xFF);
  for (int i = 0; i < 12; i++)
    msg.AddByte(p[i]);

  ESP_LOGI(TAG, "TX 130850 cmd=0x%02X dir=0x%02X angle=%u", cmd, dir, angle);
  return send_msg(msg, 0);
}

extern "C" esp_err_t n2k_send_simnet_keepalive(void)
{
  /* PGN 65305 – Simnet keypad keepalive, 8 bytes, byte[4] toggles */
  static uint8_t s_toggle = 0x03;

  tN2kMsg msg;
  msg.Init(1, PGN_SIMNET_AP_KEEPALIVE, s_nmea2000.GetN2kSource(0), 0xFF);
  msg.AddByte(0x41);
  msg.AddByte(0x9f);
  msg.AddByte(0x64);
  msg.AddByte(0x0a);
  msg.AddByte(s_toggle);
  msg.AddByte(0x00);
  msg.AddByte(0x00);
  msg.AddByte(0x00);

  s_toggle = (s_toggle == 0x03) ? 0x0b : 0x03;
  return send_msg(msg, 0);
}

extern "C" esp_err_t n2k_send_heartbeat(void)
{
  /* Heartbeat is sent automatically by the library via ParseMessages().
   * This stub keeps compatibility with the 1 Hz timer in main.c. */
  return ESP_OK;
}

extern "C" esp_err_t n2k_send_raw(uint8_t priority, uint32_t pgn, uint8_t dst, const uint8_t* data, uint8_t len, int dev)
{
  tN2kMsg msg;
  /* Source address is set by SendMsg() from the device's claimed address */
  msg.Init((unsigned char)priority, (unsigned long)pgn, 0, dst);
  for (int i = 0; i < len; i++)
    msg.AddByte(data[i]);
  return send_msg(msg, dev);
}

extern "C" void n2k_set_ap_command_handler(void (*handler)(uint8_t src, uint8_t dst, const uint8_t* data, uint8_t len))
{
  s_ap_cmd_handler = handler;
}
