/* Environmental sampling and ENS160 compensation follow the power mode. */
#include "environment_sensor.h"
#include <math.h>
#include <string.h>
#include "config.h"
#include "core.h"
#include "esp_log.h"
#include "freertos/task.h"

_Static_assert(pdMS_TO_TICKS(CFG_ENV_INTERVAL_NORMAL_MS) > 0 &&
               pdMS_TO_TICKS(CFG_ENV_INTERVAL_SAVING_MS) > 0,
               "environment intervals must be at least one tick");
static i2c_master_dev_handle_t s_sht, s_ens, s_aht;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_sensor_task;
static struct environment_reading s_environment_reading = {.error = "not_ready"};

static uint8_t sensor_crc(const uint8_t *data, size_t length) {
  uint8_t crc = 0xff;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

static esp_err_t sht_measure(float *temperature, float *humidity) {
  uint8_t command = 0xfd, data[6];
  esp_err_t err = i2c_master_transmit(s_sht, &command, 1, 50);
  if (err != ESP_OK) return err;
  vTaskDelay(pdMS_TO_TICKS(10)); /* maximum high-precision conversion: 8.3 ms */
  err = i2c_master_receive(s_sht, data, sizeof(data), 50);
  if (err != ESP_OK) return err;
  if (sensor_crc(data, 2) != data[2] || sensor_crc(data + 3, 2) != data[5])
    return ESP_ERR_INVALID_CRC;
  *temperature = -45.0f + 175.0f * ((data[0] << 8) | data[1]) / 65535.0f;
  float rh = -6.0f + 125.0f * ((data[3] << 8) | data[4]) / 65535.0f;
  *humidity = fminf(100.0f, fmaxf(0.0f, rh));
  return ESP_OK;
}

static esp_err_t aht_init(void) {
  uint8_t status;
  esp_err_t err = i2c_master_receive(s_aht, &status, 1, 50);
  if (err != ESP_OK) return err;
  if (status & 0x80) return ESP_ERR_TIMEOUT;
  if (status & 0x08) return ESP_OK;
  const uint8_t command[] = {0xbe, 0x08, 0x00};
  err = i2c_master_transmit(s_aht, command, sizeof(command), 50);
  if (err != ESP_OK) return err;
  vTaskDelay(pdMS_TO_TICKS(10));
  err = i2c_master_receive(s_aht, &status, 1, 50);
  if (err == ESP_OK && !(status & 0x08)) return ESP_ERR_INVALID_RESPONSE;
  return err;
}

static esp_err_t aht_measure(float *temperature, float *humidity) {
  const uint8_t command[] = {0xac, 0x33, 0x00};
  uint8_t data[7];
  esp_err_t err = i2c_master_transmit(s_aht, command, sizeof(command), 50);
  if (err != ESP_OK) return err;
  // Bound busy polling so a failed device cannot block other tasks.
  for (int attempt = 0; attempt < 4; ++attempt) {
    vTaskDelay(pdMS_TO_TICKS(attempt == 0 ? 80 : 10));
    err = i2c_master_receive(s_aht, data, sizeof(data), 50);
    if (err != ESP_OK) return err;
    if (data[0] & 0x80) continue;
    if (!(data[0] & 0x08)) return ESP_ERR_INVALID_RESPONSE;
    if (sensor_crc(data, 6) != data[6]) return ESP_ERR_INVALID_CRC;
    uint32_t rh = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
    uint32_t t = ((uint32_t)(data[3] & 0x0f) << 16) | ((uint32_t)data[4] << 8) | data[5];
    *humidity = rh * 100.0f / 1048576.0f;
    *temperature = t * 200.0f / 1048576.0f - 50.0f;
    return ESP_OK;
  }
  return ESP_ERR_TIMEOUT;
}

static esp_err_t ens_read(uint8_t reg, uint8_t *data, size_t size) {
  return i2c_master_transmit_receive(s_ens, &reg, 1, data, size, 50);
}

static esp_err_t ens_mode(uint8_t mode) {
  uint8_t data[] = {0x10, mode};
  return i2c_master_transmit(s_ens, data, sizeof(data), 50);
}

static esp_err_t ens_init(void) {
  uint8_t id[2];
  esp_err_t err = ens_read(0, id, sizeof(id));
  if (err != ESP_OK) return err;
  if (id[0] != 0x60 || id[1] != 0x01) return ESP_ERR_INVALID_RESPONSE;
  err = ens_mode(0x01); /* IDLE before configuration */
  if (err != ESP_OK) return err;
  vTaskDelay(pdMS_TO_TICKS(10));
  err = ens_mode(0x02); /* STANDARD gas sensing */
  vTaskDelay(pdMS_TO_TICKS(10));
  return err;
}

static esp_err_t ens_compensate(float temperature, float humidity) {
  uint16_t t = (uint16_t)lroundf((temperature + 273.15f) * 64.0f);
  uint16_t h = (uint16_t)lroundf(humidity * 512.0f);
  uint8_t data[] = {0x13, t & 0xff, t >> 8, h & 0xff, h >> 8};
  return i2c_master_transmit(s_ens, data, sizeof(data), 50);
}

void environment_sensor_get(struct environment_reading *out) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *out = s_environment_reading;
  xSemaphoreGive(s_lock);
}

static void sensor_task(void *arg) {
  (void)arg;
  bool ens_ready = false, aht_ready = false;
  unsigned ens_failures = 0;
  const char *previous_error = "not_ready";
  uint32_t last_sample = 0;
  bool first_sample = true;
  for (;;) {
    // Short waits let a switch back to standard shorten the current interval.
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool saving = s_power_saving;
    xSemaphoreGive(s_mutex);
    uint32_t interval = saving ? CFG_ENV_INTERVAL_SAVING_MS : CFG_ENV_INTERVAL_NORMAL_MS;
    uint32_t now = now_ms();
    if (!first_sample && now - last_sample < interval) {
      uint32_t remaining = interval - (now - last_sample);
      TickType_t wait = pdMS_TO_TICKS(remaining);
      ulTaskNotifyTake(pdTRUE, wait ? wait : 1);
      continue;
    }
    first_sample = false;
    last_sample = now;
    struct environment_reading next;
    environment_sensor_get(&next);
    float temperature, humidity;
    next.new_data = false;
    next.error = NULL;
    esp_err_t err = sht_measure(&temperature, &humidity);
    if (err != ESP_OK) {
      next.error = err == ESP_ERR_INVALID_CRC ? "sht40_crc_error" : "sht40_read_failed";
      goto publish;
    }
    if (!aht_ready) {
      err = aht_init();
      if (err != ESP_OK) {
        next.error = "aht21_init_failed";
        goto publish;
      }
      aht_ready = true;
    }
    float compensation_t, compensation_rh;
    err = aht_measure(&compensation_t, &compensation_rh);
    if (err != ESP_OK) {
      next.error = err == ESP_ERR_INVALID_CRC ? "aht21_crc_error" : "aht21_read_failed";
      aht_ready = false;
      goto publish;
    }
    if (!ens_ready) {
      err = ens_init();
      if (err != ESP_OK) {
        next.error = "ens160_init_failed";
        goto publish;
      }
      ens_ready = true;
    }
    err = ens_compensate(compensation_t, compensation_rh);
    if (err != ESP_OK) {
      next.error = "ens160_compensation_failed";
      goto publish;
    }
    uint8_t status;
    err = ens_read(0x20, &status, 1);
    if (err != ESP_OK) {
      next.error = "ens160_read_failed";
      goto publish;
    }
    if ((status & 0x40) || !(status & 0x80)) {
      next.error = "ens160_device_error";
      ens_ready = false;
      goto publish;
    }
    if (status & 0x02) {
      uint8_t data[5];
      err = ens_read(0x21, data, sizeof(data));
      if (err != ESP_OK) {
        next.error = "ens160_read_failed";
        goto publish;
      }
      next.aqi = data[0] & 0x07;
      next.tvoc_ppb = data[1] | (data[2] << 8);
      next.eco2_ppm = data[3] | (data[4] << 8);
      next.available = true;
      next.new_data = true;
    } else if (!next.available) {
      next.error = "not_ready";
      goto publish;
    }
    ens_failures = 0;
    next.validity = (status >> 2) & 0x03;
    next.compensation_temperature_c = compensation_t;
    next.compensation_humidity_rh_pct = compensation_rh;
    next.temperature_c = temperature;
    next.humidity_rh_pct = humidity;
    // Background NTP may not have synchronized yet.
    int64_t epoch = get_epoch_ms();
    next.updated_ms = epoch > 1000000000000LL ? epoch : 0;
    if (next.validity == 3) next.error = "ens160_invalid_output";
publish:
    // Retry a transient bus failure without restarting ENS160's warm-up.
    if (next.error && strncmp(next.error, "ens160_", 7) == 0 &&
        strcmp(next.error, "ens160_invalid_output") != 0) {
      if (++ens_failures >= 3) ens_ready = false;
    }
    if (next.error) next.new_data = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_environment_reading = next;
    xSemaphoreGive(s_lock);
    if (next.error != previous_error) {
      ESP_LOGI("environment", "sensor status: %s", next.error ? next.error : "ok");
      previous_error = next.error;
    }
    vTaskDelay(1); /* yield even if sensor work overran the sample period */
  }
}

esp_err_t environment_sensor_start(i2c_master_bus_handle_t bus) {
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) return ESP_ERR_NO_MEM;
  i2c_device_config_t config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = CFG_SHT40_ADDR,
      .scl_speed_hz = 100000,
  };
  esp_err_t err = i2c_master_bus_add_device(bus, &config, &s_sht);
  if (err != ESP_OK) goto fail;
  config.device_address = CFG_ENS160_ADDR;
  err = i2c_master_bus_add_device(bus, &config, &s_ens);
  if (err != ESP_OK) goto fail;
  config.device_address = CFG_AHT21_ADDR;
  err = i2c_master_bus_add_device(bus, &config, &s_aht);
  if (err != ESP_OK) goto fail;
  if (xTaskCreate(sensor_task, "environment", 4096, NULL, 2, &s_sensor_task) != pdPASS) {
    err = ESP_ERR_NO_MEM;
    goto fail;
  }
  return ESP_OK;
fail:
  if (s_aht) { i2c_master_bus_rm_device(s_aht); s_aht = NULL; }
  if (s_ens) { i2c_master_bus_rm_device(s_ens); s_ens = NULL; }
  if (s_sht) { i2c_master_bus_rm_device(s_sht); s_sht = NULL; }
  vSemaphoreDelete(s_lock);
  s_lock = NULL;
  return err;
}

void environment_sensor_notify_power_change(void) {
  if (s_sensor_task) xTaskNotifyGive(s_sensor_task);
}
