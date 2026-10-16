/* Periodically sample INA226 and publish local readings. */
#include "sample_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "core.h"
#include "ina226.h"
#include "temp_sensor.h"
#include "config.h"
#include "wifi_ntp.h"
#include "status.h"
#include "http_api.h"
#include "environment_sensor.h"
#include "ble_control.h"
#include "esp_log.h"

#define STACK_SAMPLE 4096
#define PRIO_SAMPLE 3

_Static_assert(pdMS_TO_TICKS(NORMAL_INTERVAL_MS) > 0 &&
               pdMS_TO_TICKS(SLOW_INTERVAL_MS) > 0,
               "sampling intervals must be at least one RTOS tick");

static uint32_t s_monitor_start;
static bool s_monitor_active;

void power_mode_monitor_start(void) {
  s_monitor_start = now_ms();
  s_monitor_active = false;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_boot_loading = true;
  xSemaphoreGive(s_mutex);
}

static void power_mode_update(float voltage) {
  bool external = voltage >= CFG_EXTERNAL_POWER_MIN_V;
  wifi_set_enabled(external);
  // Low voltage turns WiFi off; BLE remains available. external power permits connection attempts
  // while sampling/display stay in saving mode until GOT_IP.
  bool saving = !external || !wifi_is_ready();
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  bool changed = saving != s_power_saving;
  xSemaphoreGive(s_mutex);
  if (changed) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_power_saving = saving;
    nmos_control_reset();
    xSemaphoreGive(s_mutex);
    http_api_notify_power_change();
    environment_sensor_notify_power_change();
    ble_control_notify_power_change();
    set_sampling_interval(saving ? SLOW_INTERVAL_MS : NORMAL_INTERVAL_MS);
    ESP_LOGI("power-mode", "%s (%.3f V)", saving ? "ECO" : "PWR", voltage);
  }
}

static void sample_task(void *arg) {
  (void)arg;
  TickType_t last_wake = xTaskGetTickCount();
  uint32_t last_error_log = 0;
  bool read_failed = false;
  for (;;) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t interval = s_interval_ms;
    xSemaphoreGive(s_mutex);
    if (xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(interval)) == pdFALSE) {
      // Do not run a catch-up burst after a slow I2C transaction or flash write.
      last_wake = xTaskGetTickCount();
      vTaskDelay(1);
    }
    uint32_t now = now_ms();

    if (!s_monitor_active && now - s_monitor_start >= 2000u) {
      s_monitor_active = true;
      xSemaphoreTake(s_mutex, portMAX_DELAY);
      s_boot_loading = false;
      xSemaphoreGive(s_mutex);
    }
    float voltage;
    if (ina226_read_voltage(&voltage) != ESP_OK) {
      if (!read_failed || now - last_error_log >= 5000u) {
        ESP_LOGW("power-mode", "voltage read failed; using ECO until a valid reading");
        last_error_log = now;
      }
      read_failed = true;
      if (s_monitor_active)
        power_mode_update(0.0f);
      continue;
    }
    read_failed = false;
    if (s_monitor_active) power_mode_update(voltage);

    struct reading r = {
        .v = voltage,
        .i = ina226_get_current_ma(),
        .p = ina226_get_power_mw(),
        .t = temp_sensor_read_celsius(),
    };
    r.ts = get_epoch_ms(); /* stamp at read time */

    // Publish for the display + status tasks (one writer, guarded by the lock).
    reading_publish(&r);


  }
}

void sample_task_create(void) {
  ESP_ERROR_CHECK(xTaskCreate(sample_task, "sample", STACK_SAMPLE, NULL,
                             PRIO_SAMPLE, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
