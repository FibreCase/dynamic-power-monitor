/*
 * display.h — the OLED (u8g2) module. Owns the u8g2_t, the splash + live
 * frames, and the high-priority display task that redraws every 100 ms.
 */
#pragma once

#include "driver/i2c_master.h"

/* Initialise the u8g2 display (runs the SSD1306 init sequence). `oled` must
 * already be on the shared I2C bus (see init_i2c() in app_main). */
void display_init(i2c_master_dev_handle_t oled);

/* Show a one-line splash message (used during boot). */
void display_splash(const char *msg);

/* Start the display task (redraws the live frame every OLED_REFRESH_MS). */
void display_task_create(void);
