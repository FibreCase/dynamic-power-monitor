/*
 * sample_task.c — the sampling task.
 *
 * Reads the INA226 on its own period and publishes the result. It ticks every
 * 5 ms (the same cadence as the pre-refactor super-loop) and reads when
 * `now - last >= s_interval_ms`, so a host command that switches to a faster
 * rate takes effect within ~5 ms — identical to the old behaviour. The period
 * is shared with net_task via set_sampling_interval(); reading it under the
 * core mutex makes that hand-off safe.
 */
#include "sample_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "core.h"
#include "ina226.h"

#define STACK_SAMPLE 3072
#define PRIO_SAMPLE 3

static void sample_task(void *arg) {
  (void)arg;
  uint32_t last = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5));
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
