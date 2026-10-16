#pragma once
#include <stdbool.h>
#include "esp_err.h"
/* Owned by the HTTP lifecycle task; no mDNS task/timer is retained in ECO. */
esp_err_t network_mdns_set_enabled(bool enabled);
