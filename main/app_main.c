/*
 * 12V 供电动态采样监测系统 — ESP32-C3 firmware (ESP-IDF)
 *
 * Reads the INA226 power sensor, renders it on a 128x32 SSD1306 OLED, and
 * streams fixed 20-byte samples over a long-lived TCP connection to the
 * Python host. Host -> ESP32 control frames (set sampling interval) are
 * parsed from the same connection and take effect immediately.
 *
 * Shared I2C bus: INA226 (0x40) + SSD1306 OLED (0x3C).
 *
 * Wire protocol (keep in sync with the Python backend, see TASK.md):
 *   Upstream  (20 bytes, packed, little-endian):
 *     AA 55 | u64 timestamp_ms | f32 voltage(V) | f32 current(mA) | u16 checksum
 *     checksum = sum of the first 18 bytes & 0xFFFF
 *   Downstream (8 bytes, packed, little-endian):
 *     BB 66 | u8 cmd=0x01 | u8 len=0x02 | u16 interval_ms | u16 checksum
 *     checksum = sum of the first 6 bytes & 0xFFFF
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#include "ina226.h"
#include "ssd1306.h"

static const char *TAG = "power-mon";

// ---------------------------------------------------------------------------
// Configuration — edit to match your hardware / network.
// ---------------------------------------------------------------------------
#define CFG_WIFI_SSID    "CHANGE_ME_SSID"
#define CFG_WIFI_PASS    "CHANGE_ME_PASSWORD"
#define CFG_HOST_IP      "192.168.1.100"   // Python host
#define CFG_HOST_PORT    8888
#define CFG_NTP_HOST     "ntp.aliyun.com"  // SNTP server (UTC)

// I2C pins (shared bus: INA226 + SSD1306). Internal pull-ups are enabled in code.
#define CFG_I2C_SDA      GPIO_NUM_4
#define CFG_I2C_SCL      GPIO_NUM_5

// Status LED: push-pull, active HIGH (high = lit).
//   no WiFi      -> slow blink (500 ms)
//   idle (0.1Hz) -> steady on
//   fast (10 Hz) -> fast blink (100 ms)
#define CFG_LED_PIN      GPIO_NUM_8
#define CFG_LED_SLOW_MS  500u
#define CFG_LED_FAST_MS  100u

// INA226 ALERT pin (open-drain, active low). Monitored as an input.
#define CFG_ALERT_PIN    GPIO_NUM_3

// USB-JTAG: GPIO18 (D-) / GPIO19 (D+) are used by the built-in USB-Serial-JTAG.
// Do NOT repurpose these pins.
#define CFG_USB_JTAG_DMINUS  GPIO_NUM_18
#define CFG_USB_JTAG_DPLUS   GPIO_NUM_19

// INA226 calibration: shunt resistance and expected MAXIMUM current.
// The chip requires max_current * shunt <= 81.9 mV. 10 A needs shunt <= ~8.2 mOhm.
#define CFG_MAX_CURRENT_A  10.0f
#define CFG_SHUNT_OHM      0.005f          // 5 mOhm  ->  50 mV at 10 A  (OK)

// Behaviour
#define DEFAULT_INTERVAL_MS 10000u   // 0.1 Hz idle
#define FAST_INTERVAL_MS    100u     // 10 Hz
#define OLED_REFRESH_MS     100u     // hard cap: never faster
#define TCP_RETRY_MS        2000u    // reconnect period

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
static i2c_master_bus_handle_t  s_bus;
static i2c_master_dev_handle_t  s_ina, s_oled;

static esp_netif_t *s_netif;
static bool s_wifi_up = false;
static volatile bool s_wifi_disconnected = false;   // set on STA_DISCONNECTED
static bool s_ntp_synced = false;

// INA226 ALERT pin state (debounced for logging).
static volatile int  s_alert_active = 0;
static uint32_t      s_last_alert_log = 0;

static volatile uint32_t s_interval_ms = DEFAULT_INTERVAL_MS;
static char  s_mode[8] = "0.1Hz";
static float s_last_v = 0.0f, s_last_i = 0.0f, s_last_p = 0.0f;
static uint32_t s_last_sample = 0, s_last_oled = 0;

// TCP connection state.
enum { T_DISC, T_CONN, T_OK };
static int      s_sock = -1;
static int      s_tcp_state = T_DISC;
static uint32_t s_last_tcp_try = 0;

// Downstream reassembly (handles sticky / partial packets).
static uint8_t s_rx[64];
static uint8_t s_rxn = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
// 64-bit UTC Unix time in ms from the (NTP-synced) system clock.
static int64_t get_epoch_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + (int64_t)(tv.tv_usec / 1000);
}

static uint16_t byte_sum(const uint8_t *p, int n)
{
    uint32_t s = 0;
    for (int i = 0; i < n; i++) s += p[i];
    return (uint16_t)(s & 0xFFFF);
}

// ---------------------------------------------------------------------------
// Sampling mode — switches the period AND the INA226 hardware averaging.
// ---------------------------------------------------------------------------
static void set_sampling_interval(uint16_t ms)
{
    if (ms < 1) ms = DEFAULT_INTERVAL_MS;
    s_interval_ms = ms;

    if (ms <= 1000)
        ina226_set_average(INA226_AVG_64);
    else
        ina226_set_average(INA226_AVG_512);

    if (ms == FAST_INTERVAL_MS)
        snprintf(s_mode, sizeof(s_mode), "10Hz");
    else if (ms == DEFAULT_INTERVAL_MS)
        snprintf(s_mode, sizeof(s_mode), "0.1Hz");
    else
        snprintf(s_mode, sizeof(s_mode), "%ums", (unsigned)ms);

    ESP_LOGI(TAG, "sampling interval -> %u ms (%s)", (unsigned)ms, s_mode);
    s_last_sample = (uint32_t)(esp_timer_get_time() / 1000); // sample promptly
}

// ---------------------------------------------------------------------------
// TCP
// ---------------------------------------------------------------------------
static void tcp_close(void)
{
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
    s_tcp_state = T_DISC;
}

static void tcp_step(uint32_t now)
{
    switch (s_tcp_state) {
    case T_OK: {
        struct pollfd p = { .fd = s_sock, .events = POLLIN };
        int r = poll(&p, 1, 0);
        if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            ESP_LOGW(TAG, "TCP disconnected");
            tcp_close();
            return;
        }
        if (r > 0 && (p.revents & POLLIN)) {
            int n = recv(s_sock, &s_rx[s_rxn], sizeof(s_rx) - s_rxn, 0);
            if (n > 0) s_rxn = (uint8_t)(s_rxn + n);
            else if (n == 0) { tcp_close(); return; } // peer closed
        }
        break;
    }
    case T_CONN: {
        struct pollfd p = { .fd = s_sock, .events = POLLIN | POLLOUT };
        if (poll(&p, 1, 0) > 0) {
            int err = 0; socklen_t l = sizeof(err);
            getsockopt(s_sock, SOL_SOCKET, SO_ERROR, &err, &l);
            if (err == 0) { s_tcp_state = T_OK; ESP_LOGI(TAG, "TCP connected"); }
            else tcp_close();
        }
        break;
    }
    case T_DISC:
    default:
        if (now - s_last_tcp_try < TCP_RETRY_MS) return;
        s_last_tcp_try = now;

        s_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (s_sock < 0) { ESP_LOGE(TAG, "socket() failed"); return; }
        int one = 1;
        setsockopt(s_sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); // no Nagle
        int fl = fcntl(s_sock, F_GETFL, 0);
        fcntl(s_sock, F_SETFL, fl | O_NONBLOCK);

        struct sockaddr_in a = { 0 };
        a.sin_family = AF_INET;
        a.sin_port   = htons(CFG_HOST_PORT);
        inet_pton(AF_INET, CFG_HOST_IP, &a.sin_addr);

        int cr = connect(s_sock, (struct sockaddr *)&a, sizeof(a));
        if (cr == 0)            s_tcp_state = T_OK;
        else if (errno == EINPROGRESS) s_tcp_state = T_CONN;
        else { ESP_LOGE(TAG, "connect() failed: %s", strerror(errno)); close(s_sock); s_sock = -1; }
        break;
    }
}

// Parse host -> ESP32 control frames (0xBB 0x66). Little-endian fields.
static void process_downstream(void)
{
    for (;;) {
        if (s_rxn < 8) break;
        if (s_rx[0] != 0xBB || s_rx[1] != 0x66) {           // resync past bad header
            memmove(s_rx, s_rx + 1, s_rxn - 1); s_rxn--; continue;
        }
        uint16_t calc = byte_sum(s_rx, 6);
        uint16_t got  = s_rx[6] | (s_rx[7] << 8);
        if (got != calc) {                                    // checksum fail
            memmove(s_rx, s_rx + 1, s_rxn - 1); s_rxn--; continue;
        }
        uint8_t  cmd      = s_rx[2];
        uint8_t  len      = s_rx[3];
        uint16_t interval = s_rx[4] | (s_rx[5] << 8);
        if (cmd == 0x01 && len == 0x02)
            set_sampling_interval(interval);
        memmove(s_rx, s_rx + 8, s_rxn - 8); s_rxn -= 8;
    }
}

// Build and send a 20-byte upstream sample. Bytes are laid out explicitly so
// there is no struct-padding ambiguity.
static void send_sample(void)
{
    if (s_sock < 0 || s_tcp_state != T_OK) return;

    int64_t ts = get_epoch_ms();
    uint8_t buf[20];
    buf[0] = 0xAA; buf[1] = 0x55;
    for (int i = 0; i < 8; i++) buf[2 + i] = (uint8_t)((uint64_t)ts >> (8 * i)); // u64 LE

    union { float f; uint8_t b[4]; } v = { .f = s_last_v };
    union { float f; uint8_t b[4]; } c = { .f = s_last_i };
    for (int i = 0; i < 4; i++) { buf[10 + i] = v.b[i]; buf[14 + i] = c.b[i]; }

    uint16_t ck = byte_sum(buf, 18);
    buf[18] = (uint8_t)(ck & 0xFF);
    buf[19] = (uint8_t)(ck >> 8);

    (void)send(s_sock, buf, sizeof(buf), 0);
}

static void take_sample(void)
{
    s_last_v = ina226_get_voltage();
    s_last_i = ina226_get_current_ma();
    s_last_p = ina226_get_power_mw();
    send_sample();
}

// ---------------------------------------------------------------------------
// OLED
// ---------------------------------------------------------------------------
static void show_splash(const char *msg)
{
    ssd1306_clear();
    ssd1306_putstr(0, 8, msg);
    ssd1306_show();
}

static void render_oled(void)
{
    char vbuf[16], pbuf[16];
    ssd1306_clear();

    // Large voltage (top-left) and power (bottom-left), 2x scale.
    snprintf(vbuf, sizeof(vbuf), "%.2f V", s_last_v);
    ssd1306_putscaled(0, 0, vbuf, 2);
    snprintf(pbuf, sizeof(pbuf), "%.2f W", s_last_p / 1000.0f);
    ssd1306_putscaled(0, 16, pbuf, 2);

    // Right-aligned status, 1x scale: TCP state (top), sampling mode (below).
    const char *st = (s_tcp_state == T_OK) ? "TCP OK" : "DISC";
    ssd1306_putstr(128 - ssd1306_strwidth(st, 1), 0, st);
    ssd1306_putstr(128 - ssd1306_strwidth(s_mode, 1), 11, s_mode);

    ssd1306_show();
}

// ---------------------------------------------------------------------------
// Status LED + INA226 ALERT pin
// ---------------------------------------------------------------------------
// Push-pull, active HIGH. No WiFi -> slow blink, idle -> steady on, 10 Hz -> fast blink.
static void led_update(uint32_t now)
{
    bool fast = (s_interval_ms <= 1000u);
    bool wifi_ok = s_wifi_up && !s_wifi_disconnected;

    bool on;
    if (!wifi_ok)
        on = (now / CFG_LED_SLOW_MS) & 1u;   // slow blink: not connected to WiFi
    else if (fast)
        on = (now / CFG_LED_FAST_MS) & 1u;   // fast blink: 10 Hz sampling
    else
        on = true;                           // steady on: idle
    gpio_set_level(CFG_LED_PIN, on ? 1 : 0);
}

// ALERT is open-drain / active-low. Debounced: log on transition and, if held,
// periodically with the last reading so the cause (OVP/OCP/bus fault) is findable.
static void alert_poll(uint32_t now)
{
    int active = (gpio_get_level(CFG_ALERT_PIN) == 0) ? 1 : 0;
    if (active != s_alert_active) {
        s_alert_active = active;
        s_last_alert_log = now;
        if (active) {
            ESP_LOGW(TAG, "INA226 ALERT asserted");
        } else {
            ESP_LOGI(TAG, "INA226 ALERT cleared");
        }
    } else if (active && (now - s_last_alert_log > 10000u)) {
        ESP_LOGW(TAG, "INA226 ALERT held (V=%.2f I=%.0fmA P=%.1fW)",
                 s_last_v, s_last_i, s_last_p / 1000.0f);
        s_last_alert_log = now;
    }
}

static void led_init(void)
{
    gpio_config_t io = { 0 };
    io.pin_bit_mask = 1ULL << CFG_LED_PIN;
    io.mode         = GPIO_MODE_OUTPUT;
    io.pull_up_en   = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&io);
    gpio_set_level(CFG_LED_PIN, 0);
}

static void alert_gpio_init(void)
{
    gpio_config_t io = { 0 };
    io.pin_bit_mask = 1ULL << CFG_ALERT_PIN;
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;   // ALERT is open-drain; pull up when idle
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&io);
}

// ---------------------------------------------------------------------------
// WiFi + NTP
// ---------------------------------------------------------------------------
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // Link lost; the STA retries automatically. LED drops to slow blink.
        s_wifi_disconnected = true;
        ESP_LOGW(TAG, "WiFi disconnected (auto-retrying)");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_wifi_disconnected = false;
        ESP_LOGI(TAG, "WiFi connected");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_up = true;
        s_wifi_disconnected = false;
    }
}

static esp_err_t wifi_sta_start(void)
{
    esp_netif_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_mode(WIFI_MODE_STA);

    wifi_config_t sta = { 0 };
    strncpy((char *)sta.sta.ssid, CFG_WIFI_SSID, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, CFG_WIFI_PASS, sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_event_handler_instance_t inst;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, &inst));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        on_wifi_event, NULL, &inst));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi: connecting to %s ...", CFG_WIFI_SSID);
    int waited = 0;
    while (!s_wifi_up && waited < 20000) { vTaskDelay(pdMS_TO_TICKS(100)); waited += 100; }
    return s_wifi_up ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void on_sntp_sync(struct timeval *tv)
{
    (void)tv;
    s_ntp_synced = true;
    ESP_LOGI(TAG, "NTP synced");
}

static esp_err_t ntp_sync_block(void)
{
    sntp_set_time_sync_notification_cb(on_sntp_sync);
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CFG_NTP_HOST);
    esp_sntp_init();

    int waited = 0;
    while (!s_ntp_synced && waited < 30000) { vTaskDelay(pdMS_TO_TICKS(100)); waited += 100; }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec > 1000000000ULL) {
        ESP_LOGI(TAG, "NTP: epoch = %lld ms", (long long)get_epoch_ms());
        return ESP_OK;
    }
    ESP_LOGE(TAG, "NTP: not synced after timeout");
    return ESP_ERR_TIMEOUT;
}

// ---------------------------------------------------------------------------
// I2C bus + peripherals
// ---------------------------------------------------------------------------
static esp_err_t init_i2c(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port            = I2C_NUM_0,
        .sda_io_num          = CFG_I2C_SDA,
        .scl_io_num          = CFG_I2C_SCL,
        .clk_source          = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt   = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (e != ESP_OK) return e;

    i2c_device_config_t ina_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = INA226_ADDR,
        .scl_speed_hz    = 400000,
    };
    i2c_device_config_t oled_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SSD1306_ADDR,
        .scl_speed_hz    = 400000,
    };
    e = i2c_master_bus_add_device(s_bus, &ina_cfg, &s_ina);
    if (e != ESP_OK) return e;
    return i2c_master_bus_add_device(s_bus, &oled_cfg, &s_oled);
}

// ---------------------------------------------------------------------------
// app_main
// ---------------------------------------------------------------------------
void app_main(void)
{
    esp_err_t e;

    // Flash / NVS (required by the WiFi stack).
    e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    led_init();        // starts off; driven each loop iteration
    alert_gpio_init(); // INA226 ALERT as a polled input

    e = init_i2c();
    if (e != ESP_OK) { ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(e)); return; }

    e = ssd1306_init(s_oled);
    if (e != ESP_OK) ESP_LOGE(TAG, "OLED init failed: %s", esp_err_to_name(e));

    e = ina226_init(s_ina, CFG_MAX_CURRENT_A, CFG_SHUNT_OHM);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "INA226 init failed: %s (check shunt/max current)", esp_err_to_name(e));
        show_splash("INA226 FAIL");
    }

    show_splash("WiFi...");
    if (wifi_sta_start() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed");
        show_splash("WiFi FAIL");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    show_splash("NTP...");
    ntp_sync_block(); // block until the clock is synced, per spec
    vTaskDelay(pdMS_TO_TICKS(500));

    // Start in idle (0.1 Hz) mode.
    set_sampling_interval(DEFAULT_INTERVAL_MS);
    s_last_oled = 0;

    ESP_LOGI(TAG, "ready: max=%.0fA shunt=%.3fOhm interval=%ums",
             CFG_MAX_CURRENT_A, CFG_SHUNT_OHM, (unsigned)s_interval_ms);

    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        led_update(now);
        alert_poll(now);

        tcp_step(now);
        process_downstream();

        if (now - s_last_sample >= s_interval_ms) {
            s_last_sample = now;
            take_sample();
        }

        // OLED capped at 100 ms so it never starves I2C / TCP at 10 Hz.
        if (now - s_last_oled >= OLED_REFRESH_MS) {
            s_last_oled = now;
            render_oled();
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
