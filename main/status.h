/*
 * status.h — the status LED + INA226 ALERT pin module.
 */
#pragma once

/* Configure the LED (push-pull output) and the ALERT pin (pulled-up input).
 * Call before starting the status task. */
void status_init_gpio(void);

/* Start the low-priority status task (drives the LED + ALERT pin). */
void status_task_create(void);
