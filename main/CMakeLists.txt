idf_component_register(SRCS "app_main.c" "ina226.c" "ssd1306.c" "fonts.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES
                           nvs_flash
                           esp_wifi
                           esp_netif
                           esp_event
                           esp_driver_i2c
                           esp_driver_gpio
                           esp_timer
                           lwip
                           app_update
                           driver)

# config.h is git-ignored (holds WiFi credentials) and created locally from
# config.h.example. Fail at configure time with a clear message rather than a
# cryptic "CFG_WIFI_SSID undeclared" deep in the compile.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/config.h")
    message(FATAL_ERROR
        "main/config.h not found.\n"
        "  Copy the template and edit it for your hardware / network:\n"
        "    cp main/config.h.example main/config.h")
endif()
