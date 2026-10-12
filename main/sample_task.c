/* Periodically sample INA226 and publish local readings. */
#include "sample_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "core.h"
#include "ina226.h"
#include "temp_sensor.h"
#include "config.h"
#include "wifi_ntp.h"
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

void power_mode_monitor_start(void) {
  s_monitor_start = now_ms();
  s_monitor_active = false;
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

    float voltage;
    if (ina226_read_voltage(&voltage) != ESP_OK) {
      ESP_LOGW("power-mode", "voltage read failed; retaining power mode");
      continue;
    }
    if (!s_monitor_active && now - s_monitor_start >= 10000u)
      s_monitor_active = true;
    if (s_monitor_active) {
      bool battery = voltage < CFG_EXTERNAL_POWER_MIN_V;
      xSemaphoreTake(s_mutex, portMAX_DELAY);
      bool changed = battery != s_battery_mode;
      xSemaphoreGive(s_mutex);
      if (changed) {
        set_sampling_interval(battery ? SLOW_INTERVAL_MS : NORMAL_INTERVAL_MS);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_battery_mode = battery;
        xSemaphoreGive(s_mutex);
        ESP_LOGI("power-mode", "%s power (%.3f V)", battery ? "battery" : "external", voltage);
      }
      // Retry radio state changes if a previous driver call failed.
      esp_err_t err = wifi_set_enabled(!battery);
      if (err != ESP_OK)
        ESP_LOGW("power-mode", "WiFi power change failed: %s", esp_err_to_name(err));

    }

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
