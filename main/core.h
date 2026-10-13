/* Shared readings, sampling interval, WiFi state and synchronization. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* One sample. sample_task is the sole writer; display + status read it.
 * `ts` is captured at read time to record when the sample was taken. */
struct reading {
  int64_t ts; /* epoch ms */
  float v;    /* volts */
  float i;    /* mA */
  float p;    /* mW */
  float t;    /* ESP32-C3 die temperature, degC (NAN if unavailable) */
};

/* --- Shared state (defined in core.c) --- */
extern struct reading s_reading;
extern uint32_t s_interval_ms;
extern bool s_boot_loading; /* startup plus 2-second grace period; guarded by s_mutex */
extern bool s_power_saving; /* guarded by s_mutex */
extern bool s_wifi_up;
extern bool s_wifi_disconnected;
extern SemaphoreHandle_t s_mutex;  /* guards s_reading + s_interval_ms */

/* --- Control / helpers (implemented in core.c) --- */
esp_err_t core_init(void);  /* create the mutex, before tasks */

/* Switch the sampling period AND the INA226 hardware averaging. Called from
 * app_main (before tasks start) for local sampling control. */
void set_sampling_interval(uint16_t ms);

/* Thread-safe copy of the latest reading (one writer, many readers). */
void reading_publish(const struct reading *r);
void reading_get(struct reading *out);

/* Monotonic ms since boot (relative timing; wraps in ~49 days, fine here). */
uint32_t now_ms(void);

/* 64-bit UTC Unix time in ms from the (NTP-synced) system clock. */
int64_t get_epoch_ms(void);
