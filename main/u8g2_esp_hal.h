/*
 * Glue between the u8g2 graphics library and ESP-IDF's I2C master driver.
 *
 * u8g2 is platform-agnostic: it drives the SSD1306 purely through two
 * callbacks (a byte-level I2C transport and a GPIO/delay shim). This file
 * implements both against the `i2c_master_dev_handle_t` the rest of the
 * firmware already sets up for the OLED in app_main.c's init_i2c(), so the
 * display keeps sharing the same I2C bus/device as before — u8g2 itself
 * owns the SSD1306 command/addressing protocol from here on.
 */
#pragma once

#include "u8g2.h"
#include "driver/i2c_master.h"

#define OLED_I2C_ADDR 0x3C

/* Runs u8g2_Setup_ssd1306_i2c_128x32_univision_f(), wires the HAL callbacks
 * below, and performs u8g2_InitDisplay()/SetPowerSave(0)/ClearBuffer()+
 * SendBuffer() so the panel is blanked and ready to draw into. `dev` must
 * already be added to the shared I2C bus (see init_i2c()). */
void u8g2_esp_hal_init(u8g2_t *u8g2, i2c_master_dev_handle_t dev);
