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
/* Set a mode in PWR; ECO/loading returns ESP_ERR_INVALID_STATE. */
esp_err_t nmos_control_set_mode(enum nmos_mode mode);
/* API cycle is RA <-> BL; ON/OFF advance to RA. */
esp_err_t nmos_control_next_mode(enum nmos_mode *out);

/* Standard mode UI: false=power (boot default), true=environment. */
bool environment_ui_selected(void);
