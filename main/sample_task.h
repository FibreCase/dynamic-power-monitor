/*
 * sample_task.h — the sampling task: reads the INA226 on its own period and
 * publishes the result for the display + status tasks and the net task.
 */
#pragma once

/* Start the sample task. */
void sample_task_create(void);

/* Arm power monitoring 10 seconds after boot completes, before task creation. */
void power_mode_monitor_start(void);
