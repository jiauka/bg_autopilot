set(BG_AP_SRCS
    "main.c"
    "nmea2000.cpp"
    "autopilot.c"
    "n2k_log.c"
    "led_status.c"
    "led_strip_encoder.c"
)

set(BG_AP_REQUIRES
    driver
    esp_driver_rmt
    esp_timer
    freertos
    log
    nvs_flash
    bt
    NMEA2000
    NMEA2000_esp32_twai
)

if(CONFIG_BG_AP_CMD_BLE)
    list(APPEND BG_AP_SRCS "ble_ap.c")
endif()

if(CONFIG_BG_AP_CMD_SERIAL)
    list(APPEND BG_AP_SRCS "serial_cmd.c")
endif()

if(CONFIG_BG_AP_FAKE_AP)
    list(APPEND BG_AP_SRCS "fake_ap.c")
endif()

if(CONFIG_BG_AP_CMD_JOYSTICK)
    list(APPEND BG_AP_SRCS "ble_joystick.c")
endif()

idf_component_register(
    SRCS        ${BG_AP_SRCS}
    INCLUDE_DIRS "."
    REQUIRES    ${BG_AP_REQUIRES}
)
