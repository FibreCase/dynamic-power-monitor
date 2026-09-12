/*
 * core.h — shared inter-task state & control for the power-monitor firmware.
 *
 * The firmware runs several FreeRTOS tasks (net, sample, display, status).
 * They share a small set of state, all declared here and defined in core.c:
 *   - the latest published `struct reading`   (sample writes, display/status read)
 *   - the current sampling interval + the set_sampling_interval() control command
 *   - the TCP connection state                 (net writes, display/status read)
 *   - the WiFi up/disconnected flags           (wifi writes, status reads)
 *   - the shared mutex (guards reading + interval) and the length-1 sample queue
 *
 * The I2C bus and each peripheral are owned by their own modules; this header
 * only exposes the pieces that cross task boundaries.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

/* One sample. sample_task is the sole writer; display + status read it.
 * `ts` is captured at read time so the wire timestamp reflects when the sample
 * was taken, not when net_task happens to send it. */
struct reading {
  int64_t ts; /* epoch ms */
  float v;    /* volts */
  float i;    /* mA */
  float p;    /* mW */
};

/* TCP connection state. net_task writes; display + status read (volatile). */
enum tcp_state { T_DISC, T_OK };

/* --- Shared state (defined in core.c) --- */
extern struct reading s_reading;
extern uint32_t s_interval_ms;
extern volatile enum tcp_state s_tcp_state;
extern bool s_wifi_up;
extern bool s_wifi_disconnected;
extern SemaphoreHandle_t s_mutex;  /* guards s_reading + s_interval_ms */
extern QueueHandle_t s_sample_q;   /* latest sample: sample_task -> net_task */

/* --- Control / helpers (implemented in core.c) --- */
esp_err_t core_init(void);  /* create the mutex + sample queue, before tasks */

/* Switch the sampling period AND the INA226 hardware averaging. Called from
 * app_main (before tasks start) and from net_task (host command). */
void set_sampling_interval(uint16_t ms);

/* Thread-safe copy of the latest reading (one writer, many readers). */
void reading_publish(const struct reading *r);
void reading_get(struct reading *out);

/* True when in "fast" (10 Hz) sampling mode. */
bool is_fast_mode(void);

/* Monotonic ms since boot (relative timing; wraps in ~49 days, fine here). */
uint32_t now_ms(void);

/* 64-bit UTC Unix time in ms from the (NTP-synced) system clock. */
int64_t get_epoch_ms(void);
