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
#include "freertos/task.h"
#include "u8g2_esp_hal.h"

static u8g2_t s_u8g2;

void display_init(i2c_master_dev_handle_t oled) {
  u8g2_esp_hal_init(&s_u8g2, oled);
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

/* 128x64: large power, voltage/current row, then WiFi and sampling mode. */
static void oled_live_render(void) {
  struct reading r;
  bool normal;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  r = s_reading;
  normal = (s_interval_ms <= NORMAL_INTERVAL_MS);
  xSemaphoreGive(s_mutex);

  char vbuf[20], ibuf[20], pbuf[20];
  const int width = u8g2_GetDisplayWidth(&s_u8g2);
  u8g2_ClearBuffer(&s_u8g2);

  float p = r.p / 1000.0f;
  if (p < 10.0f)
    snprintf(pbuf, sizeof(pbuf), "%.2fW", p);
  else if (p < 100.0f)
    snprintf(pbuf, sizeof(pbuf), "%.1fW", p);
  else
    snprintf(pbuf, sizeof(pbuf), "%.0fW", p);
  u8g2_SetFont(&s_u8g2, u8g2_font_profont29_tf);
  if (u8g2_GetStrWidth(&s_u8g2, pbuf) > width)
    u8g2_SetFont(&s_u8g2, u8g2_font_profont12_tf);
  u8g2_DrawStr(&s_u8g2, 0, 30, pbuf);
  u8g2_DrawHLine(&s_u8g2, 0, 35, width);

  snprintf(vbuf, sizeof(vbuf), "%.2fV", r.v);
  snprintf(ibuf, sizeof(ibuf), "%.3fA", r.i / 1000.0f);
  u8g2_SetFont(&s_u8g2, u8g2_font_profont12_tf);
  // Use a smaller font if unusually long readings would overlap.
  if (u8g2_GetStrWidth(&s_u8g2, vbuf) +
      u8g2_GetStrWidth(&s_u8g2, ibuf) + 6 > width)
    u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tf);
  u8g2_DrawStr(&s_u8g2, 0, 48, vbuf);
  u8g2_DrawStr(&s_u8g2, width - u8g2_GetStrWidth(&s_u8g2, ibuf), 48, ibuf);

  const char *wifi = (!s_wifi_up || s_wifi_disconnected) ? "WiFi DISC" : "WiFi OK";
  const char *mode = normal ? "10Hz" : "1Hz";
  u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tf);
  u8g2_DrawStr(&s_u8g2, 0, 62, wifi);
  u8g2_DrawStr(&s_u8g2, width - u8g2_GetStrWidth(&s_u8g2, mode), 62, mode);

  u8g2_SendBuffer(&s_u8g2);
}

/* Redraw the live frame every OLED_REFRESH_MS (the 100 ms cap that keeps the
 * display from starving I2C at 10 Hz). */
static void display_task(void *arg) {
  (void)arg;
  for (;;) {
    oled_live_render();
    vTaskDelay(pdMS_TO_TICKS(OLED_REFRESH_MS));
  }
}

#define STACK_DISPLAY 4096 /* u8g2 full-frame buffer + string formatting */
#define PRIO_DISPLAY 4     /* highest: a u8g2 flush must never delay sampling */

void display_task_create(void) {
  xTaskCreate(display_task, "display", STACK_DISPLAY, NULL, PRIO_DISPLAY, NULL);
}
