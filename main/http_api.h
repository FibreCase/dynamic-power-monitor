#pragma once
#include "esp_err.h"

/* Background lifecycle: HTTP enabled only in PWR, stopped in ECO. */
esp_err_t http_api_control_start(void);
