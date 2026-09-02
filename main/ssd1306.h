/*
 * Minimal SSD1306 128x32 OLED driver over a shared I2C bus.
 *
 * Self-contained (no U8g2/LVGL): runs the module's init sequence, keeps a
 * 128x32 column-major frame buffer, and renders 5x7 text. Addressing uses the
 * SSD1306's horizontal mode so the buffer streams in a single I2C burst.
 *
 * Frame buffer layout: fb[col * 4 + page], page in [0,3) is the 8-row band,
 * which lines up directly with the 5x7 font (one byte per 8-row column).
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define SSD1306_ADDR  0x3C
#define SSD1306_W     128
#define SSD1306_H     32

esp_err_t ssd1306_init(i2c_master_dev_handle_t dev);

void ssd1306_clear(void);

/* Push the frame buffer to the display. */
void ssd1306_show(void);

/* Draw a string at (x, top_row) with scale 1. x in [0,127], top_row in [0,24]. */
void ssd1306_putstr(int x, int y, const char *s);

/* Draw a string scaled up by `scale` (2 => each pixel is a 2x2 block). */
void ssd1306_putscaled(int x, int y, const char *s, int scale);

/* Pixel width of a string at a given scale (5 px per char * scale). */
int ssd1306_strwidth(const char *s, int scale);
