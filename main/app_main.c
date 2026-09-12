/*
 * 12V 供电动态采样监测系统 — ESP32-C3 firmware (ESP-IDF)
 *
 * Reads the INA226 power sensor, renders it on a 128x32 SSD1306 OLED, and
 * streams fixed 20-byte samples over a long-lived TCP connection to the
 * Python host. Host -> ESP32 control frames (set sampling interval) are parsed
 * from the same connection and take effect immediately.
 *
 * This file is the composition root only: it initialises the peripherals and
 * then starts the FreeRTOS tasks that own the running behaviour. The actual
 * work lives in one module per concern:
 *
 *   core.c         shared inter-task state + the set_sampling_interval command
 *   net_task.c     TCP: connect state machine + downstream frames + sample send
 *   sample_task.c  INA226 read on its own period + publish
 *   display.c      OLED (u8g2): splash + live frame + display task
 *   status.c       status LED + INA226 ALERT pin + status task
 *   wifi_ntp.c     WiFi STA + NTP bring-up (semaphore-bounded waits)
 *   ina226.c       register-level INA226 driver
 *   u8g2_esp_hal.c u8g2 <-> ESP-IDF I2C bridge
 *
 * app_main order: NVS -> status GPIO -> I2C -> OLED -> INA226 -> WiFi -> NTP ->
 * start the four tasks -> hand the CPU to the scheduler.
 *
 * Shared I2C bus: INA226 (0x40) + SSD1306 OLED (0x3C) — the i2c_master driver
 * serialises transactions, so the display and sample tasks may share the bus.
 *
 * Wire protocol (keep in sync with the Python backend, see TASK.md):
 *   Upstream  (20 bytes, packed, little-endian):
 *     AA 55 | u64 timestamp_ms | f32 voltage(V) | f32 current(mA) | u16 checksum
 *     checksum = sum of the first 18 bytes & 0xFFFF
 *   Downstream (8 bytes, packed, little-endian):
 *     BB 66 | u8 cmd=0x01 | u8 len=0x02 | u16 interval_ms | u16 checksum
 *     checksum = sum of the first 6 bytes & 0xFFFF
 */
#include "config.h"
#include "core.h"
#include "display.h"
#include "ina226.h"
#include "net_task.h"
#include "sample_task.h"
#include "status.h"
#include "u8g2_esp_hal.h"
#include "wifi_ntp.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
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

  // Flash / NVS (required by the WiFi stack).
  e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  // Synchronisation primitives, created before any task is started.
  core_init();
  status_init_gpio(); // LED output + ALERT input

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
    display_splash("INA226 FAIL");
  }

  // WiFi: block for the connect window (GOT_IP or 20 s); on failure the module
  // runs the 2.4 GHz scan diagnostic and shows "WiFi FAIL".
  display_splash("WiFi...");
  if (wifi_boot() != ESP_OK)
    display_splash("WiFi FAIL");

  // NTP: block for the sync window (sync or 30 s). Per spec, sampling waits
  // for the clock to be synced before it starts.
  display_splash("NTP...");
  ntp_boot();
  vTaskDelay(pdMS_TO_TICKS(500));

  // Start in idle (0.1 Hz) mode.
  set_sampling_interval(DEFAULT_INTERVAL_MS);

  ESP_LOGI(TAG, "ready: max=%.0fA shunt=%.3fOhm interval=%ums",
           CFG_MAX_CURRENT_A, CFG_SHUNT_OHM, (unsigned)s_interval_ms);

  // Start the tasks. Priority order: display > sample > {net, status}.
  status_task_create();
  net_task_create();
  sample_task_create();
  display_task_create();

  // app_main's task is done; hand the CPU to the scheduler and release this
  // task's stack.
  vTaskDelete(NULL);
}
