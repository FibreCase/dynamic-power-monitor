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
#include "esp_log.h"

#define STACK_SAMPLE 4096
#define PRIO_SAMPLE 3

/* Poll cadence, in milliseconds. This MUST stay a real delay: pdMS_TO_TICKS()
 * truncates, so at CONFIG_FREERTOS_HZ < 200 it becomes 0 ticks and
 * vTaskDelay(0) only yields instead of blocking. sample_task would then spin
 * forever at PRIO_SAMPLE without ever sleeping — starving net + status (prio 2)
 * and IDLE (prio 0), which trips the task watchdog every 5 s. The build-time
 * assert below makes that failure mode impossible to reintroduce silently. */
#define SAMPLE_POLL_MS 5

_Static_assert(configTICK_RATE_HZ >= 200,
               "CONFIG_FREERTOS_HZ must be >= 200 (see sdkconfig.defaults): "
               "pdMS_TO_TICKS(SAMPLE_POLL_MS) would be 0 and vTaskDelay(0) "
               "yields rather than blocks, so sample_task would spin");

static uint32_t s_monitor_start;
static bool s_monitor_active;
static float s_last_voltage;

void power_mode_monitor_start(void) {
  s_monitor_start = now_ms();
  s_monitor_active = false;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_boot_loading = true;
  xSemaphoreGive(s_mutex);
}

static void power_mode_update(float voltage) {
  s_last_voltage = voltage;
  bool external = voltage >= CFG_EXTERNAL_POWER_MIN_V;
  wifi_set_enabled(external);
  // Low voltage turns the radio off; external power permits connection attempts
  // while sampling/display stay in saving mode until GOT_IP.
  bool saving = !external || !wifi_is_ready();
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  bool changed = saving != s_power_saving;
  xSemaphoreGive(s_mutex);
  if (changed) {
    set_sampling_interval(saving ? SLOW_INTERVAL_MS : NORMAL_INTERVAL_MS);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_power_saving = saving;
    nmos_control_reset();
    xSemaphoreGive(s_mutex);
    ESP_LOGI("power-mode", "%s (%.3f V)", saving ? "ECO" : "PWR", voltage);
  }
}

static void sample_task(void *arg) {
  (void)arg;
  uint32_t last = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(SAMPLE_POLL_MS));
    uint32_t now = now_ms();

    bool due;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    due = (now - last >= s_interval_ms);
    if (due)
      last = now;
    xSemaphoreGive(s_mutex);
    if (!due)
      continue;

    if (!s_monitor_active && now - s_monitor_start >= 2000u) {
      s_monitor_active = true;
      xSemaphoreTake(s_mutex, portMAX_DELAY);
      s_boot_loading = false;
      xSemaphoreGive(s_mutex);
    }
    float voltage;
    if (ina226_read_voltage(&voltage) != ESP_OK) {
      ESP_LOGW("power-mode", "voltage read failed; retaining power mode");
      if (s_monitor_active)
        power_mode_update(s_last_voltage);
      continue;
    }
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
  xTaskCreate(sample_task, "sample", STACK_SAMPLE, NULL, PRIO_SAMPLE, NULL);
}
