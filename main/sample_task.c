/*
 * sample_task.c — the sampling task.
 *
 * Reads the INA226 on its own period and publishes the result. It ticks every
 * SAMPLE_POLL_MS (the same cadence as the pre-refactor super-loop) and reads
 * when `now - last >= s_interval_ms`, so a host command that switches to a
 * faster rate takes effect within ~5 ms — identical to the old behaviour. The
 * period is shared with net_task via set_sampling_interval(); reading it under
 * the core mutex makes that hand-off safe.
 */
#include "sample_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "core.h"
#include "ina226.h"

#define STACK_SAMPLE 3072
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

    struct reading r = {
        .v = ina226_get_voltage(),
        .i = ina226_get_current_ma(),
        .p = ina226_get_power_mw(),
    };
    r.ts = get_epoch_ms(); /* stamp at read time */

    // Publish for the display + status tasks (one writer, guarded by the lock).
    reading_publish(&r);

    // Hand a copy to net_task to send. Overwrite the length-1 slot so only the
    // newest reading is ever available to the net task.
    xQueueOverwrite(s_sample_q, &r);
  }
}

void sample_task_create(void) {
  xTaskCreate(sample_task, "sample", STACK_SAMPLE, NULL, PRIO_SAMPLE, NULL);
}
