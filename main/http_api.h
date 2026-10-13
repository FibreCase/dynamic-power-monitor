#pragma once
#include "esp_err.h"

/* Background lifecycle: HTTP enabled only in PWR, stopped in ECO. */
esp_err_t http_api_control_start(void);

/* Notify the lifecycle worker after publishing a power-mode transition. */
void http_api_notify_power_change(void);
