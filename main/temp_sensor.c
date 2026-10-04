/*
 * temp_sensor.c — ESP32-C3 internal (die) temperature sensor (see temp_sensor.h).
 *
 * Owns the single temperature_sensor_handle_t. The driver is installed with a
 * -10..80 degC measurement range (the C3 sensor's usable span); the range only
 * selects the sensor's internal biasing, it is not a clamp.
 */
#include "temp_sensor.h"

#include <math.h>

#include "driver/temperature_sensor.h"
#include "esp_log.h"

static const char *TAG = "power-mon";

/* -10..80 degC: the ESP32-C3 internal sensor's usable range. */
#define TSENS_RANGE_MIN_C (-10)
#define TSENS_RANGE_MAX_C (80)

static temperature_sensor_handle_t s_tsens = NULL;

esp_err_t temp_sensor_init(void) {
  if (s_tsens != NULL)
    return ESP_OK;

  temperature_sensor_config_t cfg =
      TEMPERATURE_SENSOR_CONFIG_DEFAULT(TSENS_RANGE_MIN_C, TSENS_RANGE_MAX_C);
  esp_err_t e = temperature_sensor_install(&cfg, &s_tsens);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "temp sensor install failed: %s", esp_err_to_name(e));
    s_tsens = NULL;
    return e;
  }
  e = temperature_sensor_enable(s_tsens);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "temp sensor enable failed: %s", esp_err_to_name(e));
    temperature_sensor_uninstall(s_tsens);
    s_tsens = NULL;
    return e;
  }
  ESP_LOGI(TAG, "internal temp sensor ready (%d..%d C)", TSENS_RANGE_MIN_C,
           TSENS_RANGE_MAX_C);
  return ESP_OK;
}

float temp_sensor_read_celsius(void) {
  if (s_tsens == NULL)
    return NAN;
  float c = NAN;
  if (temperature_sensor_get_celsius(s_tsens, &c) != ESP_OK)
    return NAN;
  return c;
}
