/* Power monitor: local sampling/display, WiFi/NTP and HTTP-triggered OTA. */
#include "config.h"
#include "core.h"
#include "display.h"
#include "ina226.h"
#include "http_api.h"
#include "environment_sensor.h"
#include "sample_task.h"
#include "status.h"
#include "temp_sensor.h"
#include "u8g2_esp_hal.h"
#include "wifi_ntp.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "power-mon";

/* I2C bus + the two device handles. The bus is shared by the INA226 and the
 * SSD1306; each peripheral module takes ownership of its own device handle. */
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_ina, s_oled;

static esp_err_t init_i2c(void) {
  i2c_master_bus_config_t bus_cfg = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = CFG_I2C_SDA,
      .scl_io_num = CFG_I2C_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  esp_err_t e = i2c_new_master_bus(&bus_cfg, &s_bus);
  if (e != ESP_OK)
    return e;

  i2c_device_config_t ina_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = INA226_ADDR,
      .scl_speed_hz = 400000,
  };
  i2c_device_config_t oled_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = OLED_I2C_ADDR,
      .scl_speed_hz = 400000,
  };
  e = i2c_master_bus_add_device(s_bus, &ina_cfg, &s_ina);
  if (e != ESP_OK)
    return e;
  return i2c_master_bus_add_device(s_bus, &oled_cfg, &s_oled);
}

void app_main(void) {
  esp_err_t e;

  // Synchronisation primitives, created before any task is started.
  ESP_ERROR_CHECK(core_init());
  status_init_gpio(); // Radar + NMOS gates + LED output + ALERT input

  e = init_i2c();
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(e));
    return;
  }

  display_init(s_oled);

  // The INA226 may fail (wrong shunt / max current); keep running for the
  // serial log — reads degrade to 0.0 — matching the pre-refactor behaviour.
  e = ina226_init(s_ina, CFG_MAX_CURRENT_A, CFG_SHUNT_OHM);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "INA226 init failed: %s (check shunt/max current)",
             esp_err_to_name(e));
  } else {
    // Arm the shunt overcurrent comparator using Alert Limit and Mask/Enable.
    // ALERT asserts low when the threshold is exceeded.
    esp_err_t oe = ina226_set_ocp_threshold(CFG_OCP_THRESHOLD_A);
    if (oe != ESP_OK)
      ESP_LOGW(TAG, "INA226 OCP threshold not set: %s", esp_err_to_name(oe));
  }

  // Boot always uses saving mode; voltage policy is armed after startup.
  s_power_saving = true;
  set_sampling_interval(SLOW_INTERVAL_MS);

  // Flash / NVS (required by the WiFi stack).
  e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  ESP_ERROR_CHECK(wifi_background_start());

  // Internal die-temperature sensor. Best effort: if it fails to come up the
  // samples simply carry NAN and the dashboard shows no temperature.
  if (temp_sensor_init() != ESP_OK)
    ESP_LOGW(TAG, "internal temp sensor unavailable");

  ESP_LOGI(TAG, "ready: max=%.0fA shunt=%.3fOhm interval=%ums",
           CFG_MAX_CURRENT_A, CFG_SHUNT_OHM, (unsigned)s_interval_ms);

  // Confirm this app is workable so the OTA rollback machinery is coherent.
  // - A freshly-OTA'd app is in PENDING_VERIFY on first boot; this flips it to
  //   VALID so it is no longer eligible for rollback (and the next OTA is
  //   allowed). Without this the bootloader would revert to the old app.
  // - A directly-flashed app is NEW; this flips it to VALID too, so the
  //   passive slot is well-defined.
  // - An already-VALID app: no-op.
  // Local peripherals are initialized; WiFi/NTP are not boot requirements. If a bad update crashes before this line, the bootloader
  // (with app rollback enabled) reverts to the previous app automatically.
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK)
    ESP_LOGI(TAG, "OTA: running app confirmed valid (rollback armed)");
  else
    ESP_LOGW(TAG, "OTA: mark_app_valid failed (%s)", esp_err_to_name(err));

  // Start the tasks. Priority order: display > sample > status.
  status_task_create();
  ESP_ERROR_CHECK(environment_sensor_start(s_bus));
  ESP_ERROR_CHECK(http_api_control_start());
  power_mode_monitor_start();
  sample_task_create();
  display_task_create();

  // app_main's task is done; hand the CPU to the scheduler and release this
  // task's stack.
  vTaskDelete(NULL);
}
