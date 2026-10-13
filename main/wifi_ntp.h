#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Start a worker; connection and NTP synchronization never block startup. */
esp_err_t wifi_background_start(void);
/* Set desired radio state; the worker applies it asynchronously. */
esp_err_t wifi_set_enabled(bool enabled);
bool wifi_is_ready(void);
