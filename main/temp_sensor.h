/*
 * temp_sensor.h — ESP32-C3 internal (die) temperature sensor.
 *
 * A thin wrapper over the IDF `temperature_sensor` driver so the rest of the
 * firmware can read the chip's internal temperature without caring about the
 * driver's install/enable lifecycle. The reading is the *die* temperature: it
 * runs a few degrees above ambient because of the chip's own dissipation (and
 * more under WiFi load), so treat it as a relative/thermal-health signal rather
 * than an accurate ambient thermometer.
 */
#pragma once

#include "esp_err.h"

/* Install + enable the internal temperature sensor. Call once from app_main
 * before the tasks start. Idempotent; returns ESP_OK if already initialised. */
esp_err_t temp_sensor_init(void);

/* Die temperature in degrees Celsius, or NAN if the sensor is unavailable
 * (init failed / not enabled) so a sample always carries a defined value. */
float temp_sensor_read_celsius(void);
