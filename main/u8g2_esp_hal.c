#include "u8g2_esp_hal.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "U8G2_HAL";

/* u8x8_t's `user_ptr` field (and its u8x8_{Get,Set}UserPtr accessors) only
 * exist in this library on Unix/ARM-Linux builds (see the guard around
 * U8X8_WITH_USER_PTR in u8x8.h) — it's compiled out on bare-metal ESP32
 * targets. Forcing it on project-wide would work but risks an ODR mismatch
 * if it isn't defined identically for every translation unit that touches
 * u8x8_t (ours and u8g2's own). Since this firmware only ever drives one
 * OLED, it's simpler and safer to just keep the device handle in a static
 * here, set once from u8g2_esp_hal_init(). */
static i2c_master_dev_handle_t s_oled_dev;

/* Byte-level I2C transport for u8g2. u8g2/u8x8 document that they never send
 * more than 32 bytes between U8X8_MSG_BYTE_START_TRANSFER and
 * U8X8_MSG_BYTE_END_TRANSFER, so a fixed 32-byte buffer is safe: bytes are
 * accumulated here and flushed as one i2c_master_transmit() per transfer.
 * u8g2's own SSD1306 command/cad layer is what inserts the 0x00/0x40
 * control-byte prefixes and issues the addressing-mode-correct commands —
 * this callback only needs to move raw bytes over the wire. */
static uint8_t u8x8_byte_esp_i2c(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    static uint8_t buf[32];
    static uint8_t buf_len;
    (void)u8x8;

    switch (msg) {
    case U8X8_MSG_BYTE_SEND: {
        const uint8_t *data = (const uint8_t *)arg_ptr;
        while (arg_int > 0) {
            buf[buf_len++] = *data++;
            arg_int--;
        }
        break;
    }
    case U8X8_MSG_BYTE_INIT:
    case U8X8_MSG_BYTE_SET_DC:
        break;
    case U8X8_MSG_BYTE_START_TRANSFER:
        buf_len = 0;
        break;
    case U8X8_MSG_BYTE_END_TRANSFER: {
        esp_err_t e = i2c_master_transmit(s_oled_dev, buf, buf_len, 100);
        if (e != ESP_OK) ESP_LOGE(TAG, "i2c write failed: %s", esp_err_to_name(e));
        break;
    }
    default:
        return 0;
    }
    return 1;
}

/* GPIO/delay shim. This 0.91" breakout has no reset/CS/DC pins wired (I2C
 * only, control-byte addressed), so every GPIO message is a no-op — only
 * the delay messages u8g2 needs during init/refresh are implemented. */
static uint8_t u8x8_gpio_and_delay_esp(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8; (void)arg_ptr;
    switch (msg) {
    case U8X8_MSG_DELAY_MILLI:
        vTaskDelay(pdMS_TO_TICKS(arg_int));
        break;
    case U8X8_MSG_DELAY_10MICRO:
        esp_rom_delay_us(10);
        break;
    case U8X8_MSG_DELAY_100NANO:
        esp_rom_delay_us(1);
        break;
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
    default:
        break;
    }
    return 1;
}

void u8g2_esp_hal_init(u8g2_t *u8g2, i2c_master_dev_handle_t dev)
{
    s_oled_dev = dev;
    // U8G2_R2 = 180 deg rotation: this panel is mounted/wired upside down
    // relative to u8g2's default orientation.
    u8g2_Setup_ssd1306_i2c_128x32_univision_f(u8g2, U8G2_R2,
                                               u8x8_byte_esp_i2c,
                                               u8x8_gpio_and_delay_esp);

    u8g2_InitDisplay(u8g2);     // sends the SSD1306 init sequence, display left off
    u8g2_SetPowerSave(u8g2, 0); // display on
    u8g2_ClearBuffer(u8g2);
    u8g2_SendBuffer(u8g2);
}
