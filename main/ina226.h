/*
 * INA226 driver — register-level, calibrated, with mode-dependent averaging.
 *
 * Uses the shared I2C bus (a device handle for address 0x40). Calibration
 * follows the TI datasheet formula Cal = 0.00512 / (current_LSB * R_shunt),
 * with current_LSB = maxCurrent / 32768. The shunt must satisfy
 * maxCurrent * R_shunt <= 81.9 mV (the INA226's shunt range); init rejects
 * anything higher so a wrong shunt is caught immediately.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define INA226_ADDR  0x40

/* Averaging field codes (CONFIG bits [11:9]): 1,4,16,64,128,256,512,1024. */
#define INA226_AVG_64   3
#define INA226_AVG_512  6

esp_err_t ina226_init(i2c_master_dev_handle_t dev,
                      float max_current_a,
                      float shunt_ohm);

/* Bus voltage in volts (fixed 1.25 mV LSB). */
float ina226_get_voltage(void);

/* Sampled current in milliamps (signed). */
float ina226_get_current_ma(void);

/* Power in milliwatts (fixed 25 * current_LSB LSB). */
float ina226_get_power_mw(void);

/* Switch the hardware averaging window (see INA226_AVG_* above). */
void ina226_set_average(uint8_t avg_code);
