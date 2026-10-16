/*
 * status.h — the status LED + INA226 ALERT pin module.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Configure radar OUT; NMOS gates follow power-mode/radar policy; button, LED and ALERT.
 * Call before starting the status task. */
void status_init_gpio(void);

/* Start the low-priority status task (handles LED, ALERT, radar/NMOS and button). */
void status_task_create(void);

/* Read radar OUT directly: HIGH means presence. */
bool radar_presence_detected(void);

enum nmos_mode { NMOS_RA, NMOS_ON, NMOS_BL, NMOS_OFF };
/* BL is automatically reset to RA in saving mode. */
enum nmos_mode nmos_control_mode(void);
/* Called under the core mutex when power mode changes. */
void nmos_control_reset(void);
const char *nmos_control_mode_name(void);
/* One transition entry for HTTP, BLE and IO9. `mode` is input for SET and
 * output on success. PWR-only is a transport access gate, not another policy. */
enum nmos_change { NMOS_CHANGE_SET, NMOS_CHANGE_API_NEXT, NMOS_CHANGE_BUTTON_NEXT };
esp_err_t nmos_control_change(enum nmos_change change, enum nmos_mode *mode, bool pwr_only);
void nmos_control_snapshot(enum nmos_mode *mode, bool *saving, bool *loading);
/* Pure name conversion; callers with a snapshot do not need another lock. */
const char *nmos_mode_name(enum nmos_mode mode);

/* Standard mode UI: false=power (boot default), true=environment. */
bool environment_ui_selected(void);
