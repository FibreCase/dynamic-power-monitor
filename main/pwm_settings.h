#pragma once
#include <stdint.h>
#include "esp_err.h"

struct pwm_settings { uint8_t ctl1, ctl2; };
/* Load persistent duties, falling back to config defaults. Before tasks start. */
esp_err_t pwm_settings_init(void);
void pwm_settings_get(struct pwm_settings *out);
/* Commit both duties atomically; applies only after persistence succeeds. */
esp_err_t pwm_settings_save(unsigned ctl1, unsigned ctl2);
