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

/* A one-line splash message (y=20 is the text baseline in u8g2). */
void display_splash(const char *msg) {
  u8g2_ClearBuffer(&s_u8g2);
  u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tf);
  u8g2_DrawStr(&s_u8g2, 0, 20, msg);
  u8g2_SendBuffer(&s_u8g2);
}

/* The live frame: power (large, left), voltage + connection status (right).
 * 功率仍是最醒目的读数（放左边），但字号比之前小一档，好给右边的电压腾出空间。
 * 不再单独画采样模式那一行：把"快速模式"这个信息直接揉进状态文字里——
 * 10Hz 流式采样时显示 LIVE，否则显示 TCP OK；没连上就是 DISC。 */
static void oled_live_render(void) {
  struct reading r;
  bool fast;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  r = s_reading;
  fast = (s_interval_ms <= 1000u);
  xSemaphoreGive(s_mutex);

  char vbuf[16], pbuf[16];
  u8g2_ClearBuffer(&s_u8g2);

  float p = r.p / 1000.0f;
  if (p < 10.0f)
    snprintf(pbuf, sizeof(pbuf), "%.2fW", p);
  else if (p < 100.0f)
    snprintf(pbuf, sizeof(pbuf), "%.1fW", p);
  else
    snprintf(pbuf, sizeof(pbuf), "%.0fW", p);
  u8g2_SetFont(&s_u8g2, u8g2_font_profont29_tf);
  u8g2_DrawStr(&s_u8g2, 0, 25, pbuf);

  snprintf(vbuf, sizeof(vbuf), "%.2fV", r.v);
  const char *st = (s_tcp_state != T_OK) ? "DISC" : (fast ? "LIVE  " : "TCP OK");
  u8g2_SetFont(&s_u8g2, u8g2_font_profont12_tf); /* 电压：14px，比功率小一号 */
  u8g2_DrawStr(&s_u8g2, 128 - u8g2_GetStrWidth(&s_u8g2, vbuf), 15, vbuf);
  u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tf); /* 状态：最小号，贴底部 */
  u8g2_DrawStr(&s_u8g2, 128 - u8g2_GetStrWidth(&s_u8g2, st), 27, st);

  u8g2_SendBuffer(&s_u8g2);
}

/* Redraw the live frame every OLED_REFRESH_MS (the 100 ms cap that keeps the
 * display from starving I2C / TCP at 10 Hz). */
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
