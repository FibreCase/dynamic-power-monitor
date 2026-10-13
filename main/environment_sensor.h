#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

struct environment_reading {
  bool available;
  int64_t updated_ms;
  float temperature_c, humidity_rh_pct; /* SHT40 ambient readings */
  float compensation_temperature_c, compensation_humidity_rh_pct; /* AHT21 */
  uint16_t tvoc_ppb, eco2_ppm;
  uint8_t aqi, validity;
  bool new_data;
  const char *error; /* static string, or NULL */
};

esp_err_t environment_sensor_start(i2c_master_bus_handle_t bus);
void environment_sensor_get(struct environment_reading *out);

/* Wake interval scheduling after a PWR/ECO transition. */
void environment_sensor_notify_power_change(void);
