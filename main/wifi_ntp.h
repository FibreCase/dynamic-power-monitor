/*
 * wifi_ntp.h — WiFi STA bring-up and NTP sync, with FreeRTOS-bounded waits.
 */
#pragma once

#include "esp_err.h"

/* Bring up the WiFi STA (register handlers, connect), arm the connect-window
 * timer, and block until either IP_EVENT_STA_GOT_IP or the 20 s window elapses
 * (a semaphore, not a busy-poll). On success sets core's s_wifi_up; on failure
 * runs the 2.4 GHz scan diagnostic and returns ESP_ERR_TIMEOUT. */
esp_err_t wifi_boot(void);

/* Start SNTP and block until the clock is synced or the 30 s window elapses
 * (semaphore-based). Per spec, sampling waits for the synced clock. */
esp_err_t ntp_boot(void);
