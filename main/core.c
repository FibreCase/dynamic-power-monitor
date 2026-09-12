/*
 * core.c — shared inter-task state & control (see core.h).
 *
 * Owns the small set of state the FreeRTOS tasks share: the latest reading,
 * the sampling interval (plus the set_sampling_interval() command that also
 * flips the INA226 hardware averaging), the TCP/WiFi status flags, and the
 * synchronization primitives (mutex + length-1 sample queue) that keep them
 * safe.
 */
#include "core.h"

#include <sys/time.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "config.h"
#include "ina226.h"

static const char *TAG = "power-mon";

/* --- Shared state --- */
struct reading s_reading;
uint32_t s_interval_ms = DEFAULT_INTERVAL_MS;
volatile enum tcp_state s_tcp_state = T_DISC;
bool s_wifi_up = false;
bool s_wifi_disconnected = false;
SemaphoreHandle_t s_mutex;
QueueHandle_t s_sample_q; /* length 1: always carries only the latest sample */

/* --- Control / helpers --- */

// Create the synchronisation primitives. Must run before any task starts.
esp_err_t core_init(void) {
  if (!s_mutex)
    s_mutex = xSemaphoreCreateMutex();
  if (!s_sample_q)
    s_sample_q = xQueueCreate(1, sizeof(struct reading));
  return (s_mutex && s_sample_q) ? ESP_OK : ESP_ERR_NO_MEM;
}

// Switch the sampling period AND the INA226 hardware averaging.
void set_sampling_interval(uint16_t ms) {
  if (ms < 1)
    ms = DEFAULT_INTERVAL_MS;

  // Flip the INA226 averaging to match the period (64 when fast, 512 when
  // slow) — an I2C op, done outside the lock.
  if (ms <= 1000)
    ina226_set_average(INA226_AVG_64);
  else
    ina226_set_average(INA226_AVG_512);

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_interval_ms = ms;
  xSemaphoreGive(s_mutex);

  char mode[8];
  if (ms == FAST_INTERVAL_MS)
    snprintf(mode, sizeof(mode), "10Hz");
  else if (ms == DEFAULT_INTERVAL_MS)
    snprintf(mode, sizeof(mode), "0.1Hz");
  else
    snprintf(mode, sizeof(mode), "%ums", (unsigned)ms);
  ESP_LOGI(TAG, "sampling interval -> %u ms (%s)", (unsigned)ms, mode);

  // The sample task re-reads s_interval_ms every 5 ms tick, so a switch to a
  // faster period takes effect on its very next tick (~5 ms) — same as before.
}

void reading_publish(const struct reading *r) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  s_reading = *r;
  xSemaphoreGive(s_mutex);
}

void reading_get(struct reading *out) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  *out = s_reading;
  xSemaphoreGive(s_mutex);
}

bool is_fast_mode(void) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  bool fast = (s_interval_ms <= 1000u);
  xSemaphoreGive(s_mutex);
  return fast;
}

uint32_t now_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

int64_t get_epoch_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + (int64_t)(tv.tv_usec / 1000);
}
