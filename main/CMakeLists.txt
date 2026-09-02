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
