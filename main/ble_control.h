#pragma once
#include "esp_err.h"
/* BLE remains available in PWR and ECO; WiFi policy is independent. */
esp_err_t ble_control_start(void);

/* Queue power policy updates on the NimBLE host; never block sampling. */
void ble_control_notify_power_change(void);
