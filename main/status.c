/*
 * status.c — the status LED + INA226 ALERT pin.
 *
 * A single low-priority task drives both, so neither ever competes with the
 * display / sample / net tasks. LED: push-pull, active HIGH — no WiFi -> slow
 * blink, idle (0.1 Hz) -> steady on, fast (10 Hz) -> fast blink. ALERT is
 * open-drain / active-low: debounced, logged on transition and, if held,
 * periodically with the last reading so the cause (OVP/OCP/bus fault) is
 * findable.
 */
#include "status.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "core.h"
#include "ina226.h"

static const char *TAG = "power-mon";

#define STACK_STATUS 3072
#define PRIO_STATUS 2

static volatile int s_alert_active = 0;
static uint32_t s_last_alert_log = 0;

void status_init_gpio(void) {
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

static void status_task(void *arg) {
  (void)arg;
  for (;;) {
    uint32_t now = now_ms();

    // LED state.
    bool wifi_ok = s_wifi_up && !s_wifi_disconnected;
    bool on;
    if (!wifi_ok)
      on = (now / CFG_LED_SLOW_MS) & 1u; /* slow blink: no WiFi */
    else if (is_fast_mode())
      on = (now / CFG_LED_FAST_MS) & 1u; /* fast blink: 10 Hz */
    else
      on = true;                         /* steady on: idle */
    gpio_set_level(CFG_LED_PIN, on ? 1 : 0);

    // ALERT pin (open-drain, active-low). On the rising edge into asserted,
    // take a fresh INA226 read and publish an overcurrent event for net_task to
    // ship to the host; keep the existing held-alert log.
    int active = (gpio_get_level(CFG_ALERT_PIN) == 0) ? 1 : 0;
    if (active != s_alert_active) {
      s_alert_active = active;
      s_last_alert_log = now;
      if (active) {
        struct reading r = {
            .v = ina226_get_voltage(),
            .i = ina226_get_current_ma(),
            .p = ina226_get_power_mw(),
        };
        struct event ev = {
            .type = 0x01, /* shunt overcurrent */
            .ts = get_epoch_ms(),
            .v = r.v,
            .i = r.i,
            .p = r.p,
        };
        event_publish(&ev);
        ESP_LOGW(TAG, "INA226 ALERT asserted (OCP) V=%.2f I=%.0fmA P=%.1fW -> event queued",
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

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void status_task_create(void) {
  xTaskCreate(status_task, "status", STACK_STATUS, NULL, PRIO_STATUS, NULL);
}
