#pragma once
#include "esp_err.h"

#define OTA_URL_MAX_LEN 256

/* NULL uses CFG_OTA_URL; the URL is copied before returning. Non-blocking. ESP_ERR_INVALID_STATE means an update is already running. */
esp_err_t ota_update_start(const char *url);
