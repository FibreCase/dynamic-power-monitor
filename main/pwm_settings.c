#include "pwm_settings.h"
#include "config.h"
#include "core.h"
#include "nvs.h"
#include "esp_log.h"

_Static_assert(CFG_CTL1_PWM_DUTY_PCT >= 0 && CFG_CTL1_PWM_DUTY_PCT <= 100,
               "CTL1 default duty must be 0-100%");
_Static_assert(CFG_CTL2_PWM_DUTY_PCT >= 0 && CFG_CTL2_PWM_DUTY_PCT <= 100,
               "CTL2 default duty must be 0-100%");
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_save_lock;
static struct pwm_settings s_settings = {
    .ctl1 = CFG_CTL1_PWM_DUTY_PCT, .ctl2 = CFG_CTL2_PWM_DUTY_PCT,
};

esp_err_t pwm_settings_init(void) {
  s_lock = xSemaphoreCreateMutex();
  s_save_lock = xSemaphoreCreateMutex();
  if (!s_lock || !s_save_lock) return ESP_ERR_NO_MEM;
  nvs_handle_t handle;
  esp_err_t err = nvs_open("pwm", NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;
  uint8_t record[3]; /* version, CTL1, CTL2 */
  size_t length = sizeof(record);
  err = nvs_get_blob(handle, "duties", record, &length);
  nvs_close(handle);
  if (err == ESP_OK && length == sizeof(record) && record[0] == 1 &&
      record[1] <= 100 && record[2] <= 100) {
    s_settings = (struct pwm_settings){record[1], record[2]};
  } else if (err != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW("pwm", "invalid/unreadable NVS settings; using defaults");
  }
  ESP_LOGI("pwm", "duties: CTL1=%u%% CTL2=%u%%", s_settings.ctl1, s_settings.ctl2);
  return ESP_OK;
}

void pwm_settings_get(struct pwm_settings *out) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *out = s_settings;
  xSemaphoreGive(s_lock);
}

esp_err_t pwm_settings_save(unsigned ctl1, unsigned ctl2) {
  if (ctl1 > 100 || ctl2 > 100) return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  bool allowed = !s_power_saving && !s_boot_loading;
  xSemaphoreGive(s_mutex);
  if (!allowed) return ESP_ERR_INVALID_STATE;
  xSemaphoreTake(s_save_lock, portMAX_DELAY);
  struct pwm_settings current;
  pwm_settings_get(&current);
  if (current.ctl1 == ctl1 && current.ctl2 == ctl2) {
    xSemaphoreGive(s_save_lock);
    return ESP_OK; /* avoid needless flash writes */
  }
  nvs_handle_t handle;
  esp_err_t err = nvs_open("pwm", NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    const uint8_t record[] = {1, (uint8_t)ctl1, (uint8_t)ctl2};
    err = nvs_set_blob(handle, "duties", record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
  }
  if (err == ESP_OK) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_settings = (struct pwm_settings){ctl1, ctl2};
    xSemaphoreGive(s_lock);
  }
  xSemaphoreGive(s_save_lock);
  return err;
}
