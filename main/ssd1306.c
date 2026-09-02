#include "ssd1306.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "SSD1306";

/* Generated 5x7 font: 5 bytes per glyph, bit7 = top row. Chars 0x20..0x7E. */
extern const unsigned char oled_font_5x7[128 * 5];

static i2c_master_dev_handle_t s_dev;
static uint8_t s_fb[SSD1306_W * 4];   /* column-major: 4 pages of 8 rows */

/* Init sequence for the "univision" 128x32 module (same remap/scan/com pins
 * as U8g2's U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C). Addressing is page mode. */
static const uint8_t s_init_seq[] = {
    0xAE,                /* display off                       */
    0xD5, 0x80,          /* clock divide / osc frequency      */
    0xA8, 0x1F,          /* multiplex ratio (1/32)            */
    0xD3, 0x00,          /* display offset 0                  */
    0x40,                /* start line 0                      */
    0x8D, 0x14,          /* charge pump enable                */
    0x20, 0x02,          /* page addressing mode              */
    0xA1,                /* segment remap                     */
    0xC8,                /* COM scan reverse                  */
    0xDA, 0x02,          /* COM pins config, no remap         */
    0x81, 0x8F,          /* contrast                          */
    0xD9, 0xF1,          /* pre-charge period                 */
    0xDB, 0x40,          /* VCOMH deselect level              */
    0x2E,                /* no scroll                         */
    0xA4,                /* resume RAM to display             */
    0xA6,                /* normal (non-inverted) display     */
    0xAF,                /* display on                        */
};

static esp_err_t oled_write(const uint8_t *buf, size_t len)
{
    esp_err_t e = i2c_master_transmit(s_dev, buf, len, 100);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "i2c write failed: %s", esp_err_to_name(e));
    }
    return e;
}

esp_err_t ssd1306_init(i2c_master_dev_handle_t dev)
{
    s_dev = dev;
    memset(s_fb, 0, sizeof(s_fb));
    esp_err_t e = oled_write(s_init_seq, sizeof(s_init_seq));
    if (e == ESP_OK) ssd1306_show();
    return e;
}

void ssd1306_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void ssd1306_show(void)
{
    /* One burst per page: set page, set column 0..127, then 128 column bytes. */
    uint8_t out[2 + 3 + SSD1306_W];
    for (int page = 0; page < 4; page++) {
        out[0] = 0xB0 | (uint8_t)page;
        out[1] = 0x21;
        out[2] = 0x00;
        out[3] = 0x7F;
        for (int col = 0; col < SSD1306_W; col++)
            out[4 + col] = s_fb[col * 4 + page];
        oled_write(out, sizeof(out));
    }
}

static void put_pixel(int col, int row, bool on)
{
    if (col < 0 || col >= SSD1306_W || row < 0 || row >= SSD1306_H) return;
    uint8_t *b = &s_fb[col * 4 + (row >> 3)];
    uint8_t mask = (uint8_t)(1 << (7 - (row & 7)));
    if (on) *b |= mask;
    else    *b &= (uint8_t)~mask;
}

static void put_glyph_scaled(int x, int y, char c, int scale)
{
    if (c < 0x20 || c > 0x7E) return;
    const uint8_t *g = &oled_font_5x7[(c - 0x20) * 5];
    for (int cx = 0; cx < 5; cx++) {
        for (int r = 0; r < 8; r++) {
            if (((g[cx] >> (7 - r)) & 1u) == 0) continue;
            for (int dx = 0; dx < scale; dx++)
                for (int dy = 0; dy < scale; dy++)
                    put_pixel(x + cx * scale + dx, y + r * scale + dy, true);
        }
    }
}

int ssd1306_strwidth(const char *s, int scale)
{
    return (int)strlen(s) * 5 * scale;
}

void ssd1306_putstr(int x, int y, const char *s)
{
    ssd1306_putscaled(x, y, s, 1);
}

void ssd1306_putscaled(int x, int y, const char *s, int scale)
{
    if (scale < 1) scale = 1;
    int cx = x;
    for (const char *p = s; *p; p++) {
        put_glyph_scaled(cx, y, *p, scale);
        cx += 5 * scale;
    }
}
