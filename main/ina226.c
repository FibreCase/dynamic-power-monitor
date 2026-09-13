#include "ina226.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "INA226";

/* Register map (see datasheet). */
#define REG_CONFIG   0x00
#define REG_SHUNT    0x01
#define REG_BUS      0x02
#define REG_POWER    0x03
#define REG_CURRENT  0x04
#define REG_CAL      0x05
#define REG_MASK     0x06
#define REG_ALERT    0x08   /* MAR: shunt-voltage compare threshold */
#define REG_LAR      0x09   /* Latch Alert Response */
#define REG_MCP      0x0A   /* Mask / Clear / Polarity */

/* CONFIG bit fields. */
#define CFG_RESET    0x8000
#define CFG_AVG_MASK 0x0E00
#define CFG_AVG_SH   9
#define CFG_BUSV_MASK 0x01C0
#define CFG_BUSV_SH   6
#define CFG_SHV_MASK  0x0038
#define CFG_SHV_SH    3
#define CFG_MODE_MASK 0x0007
#define CFG_MODE_CC   7   /* shunt + bus continuous */

/* Datasheet limits / LSBs. */
#define SHUNT_V_MAX  0.0819f   /* 81.9 mV shunt range (0.02 mV guard) */
#define BUS_LSB_V    1.25e-3f  /* 1.25 mV */
#define CUR_FULLSCALE 32768.0f

static i2c_master_dev_handle_t s_dev;
static float s_cur_lsb;   /* amperes per current LSB (after normalization) */

static esp_err_t read_reg(uint8_t reg, uint16_t *val)
{
    uint8_t w = reg;
    uint8_t r[2];
    esp_err_t e = i2c_master_transmit_receive(s_dev, &w, 1, r, 2, 50);
    if (e == ESP_OK) *val = (uint16_t)((r[0] << 8) | r[1]);
    return e;
}

static esp_err_t write_reg(uint8_t reg, uint16_t val)
{
    uint8_t w[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    return i2c_master_transmit(s_dev, w, 3, 50);
}

esp_err_t ina226_init(i2c_master_dev_handle_t dev,
                      float max_current_a,
                      float shunt_ohm)
{
    s_dev = dev;

    if (shunt_ohm < 0.001f) {
        ESP_LOGE(TAG, "shunt %.4f ohm is below the 1 mOhm minimum", shunt_ohm);
        return ESP_ERR_INVALID_ARG;
    }
    float shunt_v = max_current_a * shunt_ohm;
    if (shunt_v > SHUNT_V_MAX) {
        ESP_LOGE(TAG, "max current %.2f A x %.4f ohm = %.1f mV exceeds the "
                     "INA226 %.1f mV shunt range — reduce max current or shunt",
                 max_current_a, shunt_ohm, shunt_v * 1000.0f, SHUNT_V_MAX * 1000.0f);
        return ESP_ERR_INVALID_ARG;
    }

    /* current_LSB = maxCurrent / 32768 (full-scale current maps to 16-bit). */
    s_cur_lsb = max_current_a / CUR_FULLSCALE;

    /* Cal = 0.00512 / (current_LSB * shunt). Keep it in the 16-bit range. */
    uint32_t cal = (uint32_t)llroundf(0.00512f / (s_cur_lsb * shunt_ohm));
    while (cal > 32767u) {
        s_cur_lsb *= 2.0f;
        cal >>= 1;
    }
    esp_err_t e = write_reg(REG_CAL, (uint16_t)cal);
    if (e != ESP_OK) { ESP_LOGE(TAG, "write CAL failed: %s", esp_err_to_name(e)); return e; }

    /* CONFIG: shunt 204us, bus 588us, continuous/continuous, 512-sample avg. */
    uint16_t cfg = (INA226_AVG_512 << CFG_AVG_SH) | (3u << CFG_BUSV_SH) | (1u << CFG_SHV_SH) | CFG_MODE_CC;
    e = write_reg(REG_CONFIG, cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "write CONFIG failed: %s", esp_err_to_name(e)); return e; }

    ESP_LOGI(TAG, "calibrated: shunt=%.4f ohm max=%.1f A cur_LSB=%.1f uA cal=%u",
             shunt_ohm, max_current_a, s_cur_lsb * 1e6f, (unsigned)cal);
    return ESP_OK;
}

float ina226_get_voltage(void)
{
    uint16_t v;
    if (read_reg(REG_BUS, &v) != ESP_OK) return 0.0f;
    return v * BUS_LSB_V;
}

float ina226_get_current_ma(void)
{
    uint16_t raw;
    if (read_reg(REG_CURRENT, &raw) != ESP_OK) return 0.0f;
    int16_t cur = (int16_t)raw;                 /* signed */
    return (cur * s_cur_lsb) * 1000.0f;
}

float ina226_get_power_mw(void)
{
    uint16_t raw;
    if (read_reg(REG_POWER, &raw) != ESP_OK) return 0.0f;
    return raw * (s_cur_lsb * 25.0f) * 1000.0f; /* power LSB = cur_LSB * 25 */
}

void ina226_set_average(uint8_t avg_code)
{
    uint16_t cfg;
    if (read_reg(REG_CONFIG, &cfg) != ESP_OK) return;
    cfg = (cfg & (uint16_t)~CFG_AVG_MASK) | ((uint16_t)avg_code << CFG_AVG_SH);
    write_reg(REG_CONFIG, cfg);
}

esp_err_t ina226_set_ocp_threshold(float threshold_a)
{
    /* The shunt comparator compares the shunt-voltage register against MAR.
     * Shunt voltage at the threshold = threshold_a * R_shunt. MAR counts
     * shunt-voltage LSBs, where shuntV_LSB = 25 * current_LSB (amperes) *
     * R_shunt (V/amp) — so R_shunt cancels and
     *     MAR = threshold_a / (25 * current_LSB).
     * (R_shunt cancels, so a wrong shunt can't skew the threshold; it's the
     * current LSB — set from max_current in init — that matters.) */
    float current_lsb = s_cur_lsb;           /* amperes per current LSB */
    uint32_t mar = (uint32_t)llroundf(threshold_a / (25.0f * current_lsb));
    if (mar == 0u) mar = 1u;                 /* guard: can't alert on 0 */

    /* MAR, then MCP (enable shunt comparator only, alert above MAR), then
     * LAR (0 = non-latched / level ALERT). Writes only to 0x08/0x0A/0x09 —
     * CAL and CONFIG are untouched, so all the reads above are unaffected. */
    esp_err_t e = write_reg(REG_ALERT, (uint16_t)mar);
    if (e == ESP_OK) e = write_reg(REG_MCP, 0x8000);
    if (e == ESP_OK) e = write_reg(REG_LAR, 0x0000);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "set OCP threshold failed: %s", esp_err_to_name(e));
        return e;
    }
    ESP_LOGI(TAG, "OCP threshold: MAR=%u (~%.2f A, shunt-only, non-latched)",
             (unsigned)mar, threshold_a);
    return ESP_OK;
}
