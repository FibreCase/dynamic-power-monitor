/*
 * status.c — the status LED + INA226 ALERT pin.
 *
 * A single low-priority task drives both, so neither ever competes with the
 * display / sample tasks. LED: active HIGH, steady on with WiFi, off without.
 * ALERT is
 * open-drain / active-low: debounced, logged on transition and, if held,
 * periodically with the last reading so the cause (OVP/OCP/bus fault) is
 * findable.
 */
#include "status.h"
#include <stdatomic.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "core.h"
#include "ina226.h"
#include "pwm_settings.h"
#include "wifi_ntp.h"

static const char *TAG = "power-mon";

#define STACK_STATUS 3072
#define PRIO_STATUS 3

static atomic_bool s_environment_ui = ATOMIC_VAR_INIT(false);
static atomic_int s_nmos_mode = ATOMIC_VAR_INIT(NMOS_RA);

static volatile int s_alert_active = 0;
static uint32_t s_last_alert_log = 0;

#define PWM_MODE LEDC_LOW_SPEED_MODE
#define PWM_TIMER LEDC_TIMER_0
#define PWM_RESOLUTION LEDC_TIMER_8_BIT
static const ledc_channel_t s_pwm_channels[] = {LEDC_CHANNEL_0, LEDC_CHANNEL_1};

static void pwm_init(void) {
  const ledc_timer_config_t timer = {
      .speed_mode = PWM_MODE,
      .duty_resolution = PWM_RESOLUTION,
      .timer_num = PWM_TIMER,
      .freq_hz = CFG_NMOS_PWM_FREQ_HZ,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  ESP_ERROR_CHECK(ledc_timer_config(&timer));
  const int pins[] = {CFG_NMOS_CTL1_PIN, CFG_NMOS_CTL2_PIN};
  for (unsigned i = 0; i < 2; ++i) {
    const ledc_channel_config_t channel = {
        .gpio_num = pins[i], .speed_mode = PWM_MODE,
        .channel = s_pwm_channels[i], .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = PWM_TIMER, .duty = 0, .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
  }
}

static void pwm_set(unsigned output, unsigned duty_pct) {
  static int previous[] = {-1, -1};
  if (previous[output] == (int)duty_pct) return;
  ledc_channel_t channel = s_pwm_channels[output];
  if (duty_pct == 0 || duty_pct == 100) {
    ESP_ERROR_CHECK(ledc_stop(PWM_MODE, channel, duty_pct == 100));
  } else {
    unsigned duty = (256u * duty_pct + 50u) / 100u;
    ESP_ERROR_CHECK(ledc_set_duty(PWM_MODE, channel, duty));
    ESP_ERROR_CHECK(ledc_update_duty(PWM_MODE, channel));
  }
  previous[output] = duty_pct;
}

static void nmos_set_outputs(bool presence) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  bool saving = s_power_saving;
  enum nmos_mode mode = atomic_load(&s_nmos_mode);
  xSemaphoreGive(s_mutex);
  bool ctl1 = mode != NMOS_OFF && (mode == NMOS_ON || presence);
  bool ctl2 = mode == NMOS_ON || (mode == NMOS_RA && !saving && presence);
  struct pwm_settings settings;
  pwm_settings_get(&settings);
  bool adjustable = !saving && mode != NMOS_ON;
  pwm_set(0, !ctl1 ? 0u : adjustable ? settings.ctl1 : 100u);
  pwm_set(1, !ctl2 ? 0u : adjustable ? settings.ctl2 : 100u);
}

void status_init_gpio(void) {
  // Pull radar OUT low when it is not driven.
  gpio_config_t radar = {
      .pin_bit_mask = 1ULL << CFG_RADAR_OUT_PIN,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_ENABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&radar));

  // Button closes to GND; released HIGH, pressed LOW.
  gpio_config_t button = {
      .pin_bit_mask = 1ULL << CFG_BUTTON_PIN,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&button));

  // Boot is saving mode: CTL1 follows radar, CTL2 off, unless overridden.
  pwm_init();
  nmos_set_outputs(radar_presence_detected());



  // LED: push-pull output, no pull, no interrupt.
  gpio_config_t led = {0};
  led.pin_bit_mask = 1ULL << CFG_LED_PIN;
  led.mode = GPIO_MODE_OUTPUT;
  led.pull_up_en = GPIO_PULLUP_DISABLE;
  led.pull_down_en = GPIO_PULLDOWN_DISABLE;
  led.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&led);
  gpio_set_level(CFG_LED_PIN, 0);

  // ALERT: input, pulled up (the pin is open-drain; high when idle).
  gpio_config_t alert = {0};
  alert.pin_bit_mask = 1ULL << CFG_ALERT_PIN;
  alert.mode = GPIO_MODE_INPUT;
  alert.pull_up_en = GPIO_PULLUP_ENABLE;
  alert.pull_down_en = GPIO_PULLDOWN_DISABLE;
  alert.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&alert);
}

bool radar_presence_detected(void) {
  return gpio_get_level(CFG_RADAR_OUT_PIN) != 0;
}

void nmos_control_reset(void) {
  atomic_store(&s_nmos_mode, NMOS_RA);
}

enum nmos_mode nmos_control_mode(void) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  enum nmos_mode mode = atomic_load(&s_nmos_mode);
  xSemaphoreGive(s_mutex);
  return mode;
}

const char *nmos_control_mode_name(void) {
  switch (nmos_control_mode()) {
  case NMOS_ON: return "ON";
  case NMOS_BL: return "BL";
  case NMOS_OFF: return "OFF";
  default: return "RA";
  }
}

esp_err_t nmos_control_set_mode(enum nmos_mode mode) {
  if (mode != NMOS_RA && mode != NMOS_ON && mode != NMOS_BL && mode != NMOS_OFF)
    return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  if (s_power_saving || s_boot_loading) {
    xSemaphoreGive(s_mutex);
    return ESP_ERR_INVALID_STATE;
  }
  atomic_store(&s_nmos_mode, mode);
  xSemaphoreGive(s_mutex);
  ESP_LOGI(TAG, "NMOS API mode: %s", mode == NMOS_RA ? "RA" : mode == NMOS_ON ? "ON" : mode == NMOS_BL ? "BL" : "OFF");
  return ESP_OK;
}

esp_err_t nmos_control_next_mode(enum nmos_mode *out) {
  if (!out) return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  if (s_power_saving || s_boot_loading) {
    xSemaphoreGive(s_mutex);
    return ESP_ERR_INVALID_STATE;
  }
  int current = atomic_load(&s_nmos_mode);
  *out = current == NMOS_RA ? NMOS_BL : NMOS_RA;
  atomic_store(&s_nmos_mode, *out);
  xSemaphoreGive(s_mutex);
  return ESP_OK;
}

bool environment_ui_selected(void) {
  return atomic_load(&s_environment_ui);
}

static void button_long_press(void) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  bool standard = !s_power_saving && !s_boot_loading;
  xSemaphoreGive(s_mutex);
  if (standard) {
    bool environment = !environment_ui_selected();
    atomic_store(&s_environment_ui, environment);
    ESP_LOGI(TAG, "UI: %s", environment ? "environment" : "power");
  }
}

static void status_task(void *arg) {
  (void)arg;
  int last_presence = -1;
  bool button_candidate = gpio_get_level(CFG_BUTTON_PIN) == 0;
  bool button_pressed = button_candidate;
  uint32_t button_changed_at = now_ms();
  uint32_t button_pressed_at = button_changed_at;
  bool long_handled = button_pressed; /* ignore a key already held at boot */
  ESP_LOGI(TAG, "Button IO9: %s", button_pressed ? "pressed" : "released");
  TickType_t last_wake = xTaskGetTickCount();
  for (;;) {
    uint32_t now = now_ms();

    bool raw_pressed = gpio_get_level(CFG_BUTTON_PIN) == 0;
    if (raw_pressed != button_candidate) {
      button_candidate = raw_pressed;
      button_changed_at = now;
    }
    if (button_candidate != button_pressed &&
        now - button_changed_at >= CFG_BUTTON_DEBOUNCE_MS) {
      button_pressed = button_candidate;
      ESP_LOGI(TAG, "Button IO9: %s", button_pressed ? "pressed" : "released");
      if (button_pressed) {
        button_pressed_at = now;
        long_handled = false;
      } else if (!long_handled) {
        if (now - button_pressed_at >= CFG_BUTTON_LONG_PRESS_MS) {
          button_long_press();
        } else {
          xSemaphoreTake(s_mutex, portMAX_DELAY);
          bool saving = s_power_saving;
          enum nmos_mode current = atomic_load(&s_nmos_mode);
          enum nmos_mode next = current == NMOS_RA ? NMOS_ON :
              current == NMOS_ON ? (saving ? NMOS_OFF : NMOS_BL) :
              current == NMOS_BL ? NMOS_OFF : NMOS_RA;
          atomic_store(&s_nmos_mode, next);
          xSemaphoreGive(s_mutex);
          ESP_LOGI(TAG, "NMOS mode: %s", nmos_control_mode_name());
        }
      }
    }
    if (button_pressed && button_candidate && !long_handled &&
        now - button_pressed_at >= CFG_BUTTON_LONG_PRESS_MS) {
      long_handled = true;
      button_long_press();
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool saving = s_power_saving;
    xSemaphoreGive(s_mutex);
    int presence = radar_presence_detected();
    nmos_set_outputs(presence);
    if (presence != last_presence) {
      last_presence = presence;
      ESP_LOGI(TAG, "Radar: %s", presence ? "occupied" : "empty");
    }

    // Saving mode forces the LED off, even while WiFi is reconnecting.
    bool wifi_ok = wifi_is_ready();
    gpio_set_level(CFG_LED_PIN, !saving && wifi_ok ? 1 : 0);

    // Log overcurrent transitions locally.
    int active = (gpio_get_level(CFG_ALERT_PIN) == 0) ? 1 : 0;
    if (active != s_alert_active) {
      s_alert_active = active;
      s_last_alert_log = now;
      if (active) {
        struct reading r;
        reading_get(&r); /* cached sample keeps button/PWM polling responsive */
        ESP_LOGW(TAG, "INA226 ALERT asserted (OCP) V=%.2f I=%.0fmA P=%.1fW",
                 r.v, r.i, r.p / 1000.0f);
      } else {
        ESP_LOGI(TAG, "INA226 ALERT cleared");
      }
    } else if (active && (now - s_last_alert_log > 10000u)) {
      struct reading r;
      reading_get(&r);
      ESP_LOGW(TAG, "INA226 ALERT held (V=%.2f I=%.0fmA P=%.1fW)", r.v, r.i,
               r.p / 1000.0f);
      s_last_alert_log = now;
    }

    if (xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(50)) == pdFALSE) {
      last_wake = xTaskGetTickCount();
      vTaskDelay(1);
    }
  }
}

void status_task_create(void) {
  ESP_ERROR_CHECK(xTaskCreate(status_task, "status", STACK_STATUS, NULL,
                             PRIO_STATUS, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
