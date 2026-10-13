/*
 * display.c — the OLED module (u8g2).
 *
 * Owns the u8g2_t and is the sole caller of its draw/send functions: the boot
 * splash frames (drawn synchronously from app_main) and the live frame
 * (drawn by the display task every OLED_REFRESH_MS). Owning the frame buffer
 * in one place means no locking is needed around u8g2 — the only other
 * readers of shared state are copied under the core mutex before a frame is
 * drawn.
 */
#include "display.h"

#include <stdio.h>

#include "config.h"
#include "core.h"
#include "status.h"
#include "environment_sensor.h"
#include "freertos/task.h"
#include "u8g2_esp_hal.h"

static u8g2_t s_u8g2;

void display_init(i2c_master_dev_handle_t oled) {
  u8g2_esp_hal_init(&s_u8g2, oled);
  u8g2_SetContrast(&s_u8g2, CFG_OLED_CONTRAST_BATTERY);
  display_splash("Loading...");
}

/* Center the boot message on the panel. */
void display_splash(const char *msg) {
  u8g2_ClearBuffer(&s_u8g2);
  u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tf);
  int x = ((int)u8g2_GetDisplayWidth(&s_u8g2) -
           (int)u8g2_GetStrWidth(&s_u8g2, msg)) / 2;
  u8g2_DrawStr(&s_u8g2, x > 0 ? x : 0, 36, msg);
  u8g2_SendBuffer(&s_u8g2);
}

/* Both screens share a readable power/control and AQI footer. */
static void oled_footer_render(const struct environment_reading *r) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  bool saving = s_power_saving;
  xSemaphoreGive(s_mutex);
  char left[24], right[16];
  snprintf(left, sizeof(left), "%s | %s", saving ? "ECO" : "PWR",
           nmos_control_mode_name());
  if (r->available)
    snprintf(right, sizeof(right), "AQI %u", (unsigned)r->aqi);
  else
    snprintf(right, sizeof(right), "AQI --");
  int width = u8g2_GetDisplayWidth(&s_u8g2);
  u8g2_SetFont(&s_u8g2, u8g2_font_profont12_tf);
  u8g2_DrawStr(&s_u8g2, 0, 62, left);
  u8g2_DrawStr(&s_u8g2, width - u8g2_GetStrWidth(&s_u8g2, right), 62, right);
}

/* Environmental readings with the shared mode/AQI footer. */
static void oled_environment_render(void) {
  struct environment_reading r;
  environment_sensor_get(&r);
  char temperature[24], humidity[24], co2[24], tvoc[24];
  if (r.available) {
    snprintf(temperature, sizeof(temperature), "%.1fC", r.temperature_c);
    snprintf(humidity, sizeof(humidity), "%.1f%%", r.humidity_rh_pct);
    snprintf(co2, sizeof(co2), "%u", (unsigned)r.eco2_ppm);
    snprintf(tvoc, sizeof(tvoc), "TVOC %u", (unsigned)r.tvoc_ppb);
  } else {
    snprintf(temperature, sizeof(temperature), "--C");
    snprintf(humidity, sizeof(humidity), "--%%");
    snprintf(co2, sizeof(co2), "--");
    snprintf(tvoc, sizeof(tvoc), "TVOC --");
  }
  const int width = u8g2_GetDisplayWidth(&s_u8g2);
  u8g2_ClearBuffer(&s_u8g2);
  u8g2_SetFont(&s_u8g2, u8g2_font_profont17_tf);
  int co2_width = u8g2_GetStrWidth(&s_u8g2, co2);
  u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tf);
  const char *co2_label = "ppm";
  int label_width = u8g2_GetStrWidth(&s_u8g2, co2_label);
  int right_width = co2_width > label_width ? co2_width : label_width;
  u8g2_DrawStr(&s_u8g2, width - label_width, 10, co2_label);
  u8g2_SetFont(&s_u8g2, u8g2_font_profont17_tf);
  u8g2_DrawStr(&s_u8g2, width - co2_width, 28, co2);
  // Strike through gas values until ENS160 completes warm-up/startup.
  if (r.available && (r.validity == 1 || r.validity == 2)) {
    int line_y = 28 - u8g2_GetAscent(&s_u8g2) / 2;
    u8g2_DrawHLine(&s_u8g2, width - co2_width, line_y, co2_width);
  }

  u8g2_SetFont(&s_u8g2, u8g2_font_profont29_tf);
  if (u8g2_GetStrWidth(&s_u8g2, temperature) > width - right_width - 6)
    u8g2_SetFont(&s_u8g2, u8g2_font_profont22_tf);
  if (u8g2_GetStrWidth(&s_u8g2, temperature) > width - right_width - 6)
    u8g2_SetFont(&s_u8g2, u8g2_font_profont12_tf);
  u8g2_DrawStr(&s_u8g2, 0, 30, temperature);
  u8g2_DrawHLine(&s_u8g2, 0, 35, width);

  u8g2_SetFont(&s_u8g2, u8g2_font_profont12_tf);
  u8g2_DrawStr(&s_u8g2, 0, 48, humidity);
  u8g2_DrawStr(&s_u8g2, width - u8g2_GetStrWidth(&s_u8g2, tvoc), 48, tvoc);
  oled_footer_render(&r);
  u8g2_SendBuffer(&s_u8g2);
}

/* 128x64: large power, voltage/current row, then WiFi and sampling mode. */
static void oled_live_render(void) {
  struct reading r;
  bool saving;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  r = s_reading;
  saving = s_power_saving;
  xSemaphoreGive(s_mutex);

  static int contrast = -1;
  int desired = saving ? CFG_OLED_CONTRAST_BATTERY : CFG_OLED_CONTRAST_NORMAL;
  if (contrast != desired) {
    u8g2_SetContrast(&s_u8g2, desired);
    contrast = desired;
  }

  if (saving || environment_ui_selected()) {
    oled_environment_render();
    return;
  }

  char electrical[40], tvoc[24], pbuf[20], voltage[20];
  const int width = u8g2_GetDisplayWidth(&s_u8g2);
  u8g2_ClearBuffer(&s_u8g2);

  float p = r.p / 1000.0f;
  if (p < 10.0f)
    snprintf(pbuf, sizeof(pbuf), "%.2fW", p);
  else if (p < 100.0f)
    snprintf(pbuf, sizeof(pbuf), "%.1fW", p);
  else
    snprintf(pbuf, sizeof(pbuf), "%.0fW", p);
  snprintf(voltage, sizeof(voltage), "%.2f", r.v);
  u8g2_SetFont(&s_u8g2, u8g2_font_profont17_tf);
  int voltage_width = u8g2_GetStrWidth(&s_u8g2, voltage);
  u8g2_DrawStr(&s_u8g2, width - voltage_width, 28, voltage);
  u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tf);
  u8g2_DrawStr(&s_u8g2, width - u8g2_GetStrWidth(&s_u8g2, "V"), 10, "V");
  u8g2_SetFont(&s_u8g2, u8g2_font_profont29_tf);
  if (u8g2_GetStrWidth(&s_u8g2, pbuf) > width - voltage_width - 6)
    u8g2_SetFont(&s_u8g2, u8g2_font_profont22_tf);
  if (u8g2_GetStrWidth(&s_u8g2, pbuf) > width - voltage_width - 6)
    u8g2_SetFont(&s_u8g2, u8g2_font_profont12_tf);
  u8g2_DrawStr(&s_u8g2, 0, 30, pbuf);
  u8g2_DrawHLine(&s_u8g2, 0, 35, width);

  struct environment_reading environment;
  environment_sensor_get(&environment);
  snprintf(electrical, sizeof(electrical), "%.2fA", r.i / 1000.0f);
  if (environment.available)
    snprintf(tvoc, sizeof(tvoc), "TVOC %u", (unsigned)environment.tvoc_ppb);
  else
    snprintf(tvoc, sizeof(tvoc), "TVOC --");
  u8g2_SetFont(&s_u8g2, u8g2_font_profont12_tf);
  if (environment.available &&
      u8g2_GetStrWidth(&s_u8g2, electrical) + u8g2_GetStrWidth(&s_u8g2, tvoc) + 2 > width)
    snprintf(tvoc, sizeof(tvoc), "TVOC %uk", ((unsigned)environment.tvoc_ppb + 500u) / 1000u);
  u8g2_DrawStr(&s_u8g2, 0, 48, electrical);
  u8g2_DrawStr(&s_u8g2, width - u8g2_GetStrWidth(&s_u8g2, tvoc), 48, tvoc);

  oled_footer_render(&environment);

  u8g2_SendBuffer(&s_u8g2);
}

/* Redraw the live frame every OLED_REFRESH_MS (the 100 ms cap that keeps the
 * display from starving I2C at 10 Hz). */
static void display_task(void *arg) {
  (void)arg;
  for (;;) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool loading = s_boot_loading;
    xSemaphoreGive(s_mutex);
    if (loading)
      display_splash("Loading...");
    else
      oled_live_render();
    vTaskDelay(pdMS_TO_TICKS(OLED_REFRESH_MS));
  }
}

#define STACK_DISPLAY 4096 /* u8g2 full-frame buffer + string formatting */
#define PRIO_DISPLAY 4     /* highest: a u8g2 flush must never delay sampling */

void display_task_create(void) {
  xTaskCreate(display_task, "display", STACK_DISPLAY, NULL, PRIO_DISPLAY, NULL);
}
