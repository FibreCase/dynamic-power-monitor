/*
 * wifi_ntp.c — WiFi STA bring-up + NTP sync.
 *
 * Both boot steps use a one-shot FreeRTOS software timer + binary semaphore to
 * bound their wait (GOT_IP / the sync notification gives the semaphore on
 * success; the timer gives it at the deadline). The timer callback runs in the
 * FreeRTOS timer daemon task — not the ESP event handler or the SNTP callback
 * context — so it is safe to xSemaphoreGive(). app_main is released by the
 * semaphore instead of busy-polling a flag.
 *
 * The WiFi status flags shared with the status task (s_wifi_up /
 * s_wifi_disconnected) live in core.c; the connect-window timer and the two
 * wait semaphores are private to this file.
 */
#include "wifi_ntp.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "config.h"
#include "core.h"

static const char *TAG = "power-mon";

#define WIFI_CONNECT_WINDOW_MS 20000u /* app_main waits this long for an IP */
#define NTP_SYNC_WINDOW_MS 30000u     /* app_main waits this long for a sync */

static esp_netif_t *s_netif;
static TimerHandle_t s_wifi_timer;
static SemaphoreHandle_t s_wifi_sem;
static SemaphoreHandle_t s_ntp_sem;

/* --- WiFi: reason labels --- */
static const char *wifi_reason_str(uint8_t reason) {
  switch (reason) {
  case WIFI_REASON_NO_AP_FOUND:
    return "no AP with this SSID (check SSID / 2.4 GHz band)";
  case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    return "no AP with compatible security (authmode mismatch)";
  case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    return "no AP for configured authmode threshold";
  case WIFI_REASON_AUTH_FAIL:
    return "authentication failed (wrong password?)";
  case WIFI_REASON_HANDSHAKE_TIMEOUT:
    return "WPA handshake timeout (authmode mismatch)";
  case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    return "4-way handshake timeout (authmode mismatch)";
  case WIFI_REASON_ASSOC_FAIL:
    return "association failed";
  case WIFI_REASON_ASSOC_TOOMANY:
    return "AP: too many associated stations";
  case WIFI_REASON_BEACON_TIMEOUT:
    return "beacon timeout";
  case WIFI_REASON_CONNECTION_FAIL:
    return "connection failed";
  default:
    return "other";
  }
}

/* Connect-window deadline: runs in the timer daemon task, so it can safely
 * give the semaphore app_main waits on. */
static void wifi_timer_cb(TimerHandle_t t) {
  (void)t;
  if (s_wifi_sem)
    xSemaphoreGive(s_wifi_sem);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id,
                          void *data) {
  (void)arg;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    // Link lost; the STA retries automatically. LED drops to slow blink.
    const wifi_event_sta_disconnected_t *d = data;
    s_wifi_disconnected = true;
    ESP_LOGW(TAG, "WiFi disconnected: reason=%u (%s)", d->reason,
             wifi_reason_str(d->reason));
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
    s_wifi_disconnected = false;
    ESP_LOGI(TAG, "WiFi connected");
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    s_wifi_up = true;
    s_wifi_disconnected = false;
    if (s_wifi_timer)
      xTimerStop(s_wifi_timer, 0);
    if (s_wifi_sem)
      xSemaphoreGive(s_wifi_sem);
  }
}

/* Diagnostic: scan all 2.4 GHz APs this device can see and print SSID /
 * channel / RSSI / authmode. C3 is 2.4 GHz only, so this is the definitive way
 * to confirm the router's SSID is visible and its security mode (WPA2-PSK =
 * 3). Run while the driver is started, so it is called after a failed
 * connect window. */
static void wifi_scan_dump(void) {
  wifi_scan_config_t scan_cfg = {0};
  scan_cfg.show_hidden = false;
  scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  scan_cfg.scan_time.active.max = 100;
  esp_wifi_scan_start(&scan_cfg, true);

  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  ESP_LOGI(TAG, "=== WiFi scan: %u AP(s) visible ===", (unsigned)n);

  wifi_ap_record_t *aps = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
  if (aps) {
    uint16_t got = n;
    if (esp_wifi_scan_get_ap_records(&got, aps) == ESP_OK) {
      for (int i = 0; i < got; i++) {
        char ssid[33] = {0};
        memcpy(ssid, aps[i].ssid, strnlen((const char *)aps[i].ssid, 32));
        const char *mark =
            (strcmp(ssid, CFG_WIFI_SSID) == 0) ? "  <-- target" : "";
        ESP_LOGI(TAG, "  ch=%-2u rssi=%-3d auth=%u [%s]%s",
                 (unsigned)aps[i].primary, (int)aps[i].rssi,
                 (unsigned)aps[i].authmode, ssid, mark);
      }
    }
    free(aps);
  }
  esp_wifi_clear_ap_list();
}

/* Register handlers, bring up STA mode, and initiate the connect. Does NOT
 * wait for the IP — wifi_boot() waits on s_wifi_sem. */
static esp_err_t wifi_sta_start(void) {
  esp_netif_init();
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  s_netif = esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  esp_wifi_set_mode(WIFI_MODE_STA);

  wifi_config_t sta = {0};
  strncpy((char *)sta.sta.ssid, CFG_WIFI_SSID, sizeof(sta.sta.ssid) - 1);
  strncpy((char *)sta.sta.password, CFG_WIFI_PASS,
          sizeof(sta.sta.password) - 1);
  // Do NOT force sta.sta.threshold.authmode: leaving it 0 (WIFI_AUTH_UNKNOWN)
  // lets the driver auto-detect WPA2 vs WPA3 vs mixed.

  esp_event_handler_instance_t inst;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, &inst));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, &inst));

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_connect());

  ESP_LOGI(TAG, "WiFi: connecting to %s ...", CFG_WIFI_SSID);
  return ESP_OK;
}

esp_err_t wifi_boot(void) {
  s_wifi_sem = xSemaphoreCreateBinary();
  s_wifi_timer = xTimerCreate("wifi", pdMS_TO_TICKS(WIFI_CONNECT_WINDOW_MS),
                              pdFALSE, NULL, wifi_timer_cb);
  if (s_wifi_timer)
    xTimerStart(s_wifi_timer, 0);

  wifi_sta_start();

  // Block until GOT_IP gave the semaphore or the connect window elapsed.
  xSemaphoreTake(s_wifi_sem, pdMS_TO_TICKS(WIFI_CONNECT_WINDOW_MS + 500));
  if (s_wifi_timer) {
    xTimerDelete(s_wifi_timer, 0);
    s_wifi_timer = NULL;
  }
  if (s_wifi_sem) {
    vSemaphoreDelete(s_wifi_sem);
    s_wifi_sem = NULL;
  }

  if (!s_wifi_up) {
    ESP_LOGE(TAG, "WiFi connect failed");
    wifi_scan_dump();
    vTaskDelay(pdMS_TO_TICKS(2000));
    return ESP_ERR_TIMEOUT;
  }
  return ESP_OK;
}

/* --- NTP --- */
static void on_sntp_sync(struct timeval *tv) {
  (void)tv;
  if (s_ntp_sem)
    xSemaphoreGive(s_ntp_sem);
}

/* Sync-window deadline: runs in the timer daemon task; logs the final
 * (synced-or-not) epoch and releases app_main regardless. */
static void ntp_timer_cb(TimerHandle_t t) {
  (void)t;
  struct timeval tv;
  gettimeofday(&tv, NULL);
  if (tv.tv_sec > 1000000000ULL)
    ESP_LOGI(TAG, "NTP: synced, epoch = %lld ms", (long long)get_epoch_ms());
  else
    ESP_LOGE(TAG, "NTP: not synced after timeout");
  if (s_ntp_sem)
    xSemaphoreGive(s_ntp_sem);
}

static void ntp_start(void) {
  sntp_set_time_sync_notification_cb(on_sntp_sync);
  sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, CFG_NTP_HOST);
  esp_sntp_init();
}

esp_err_t ntp_boot(void) {
  s_ntp_sem = xSemaphoreCreateBinary();
  TimerHandle_t ntp_timer = xTimerCreate(
      "ntp", pdMS_TO_TICKS(NTP_SYNC_WINDOW_MS), pdFALSE, NULL, ntp_timer_cb);
  if (ntp_timer)
    xTimerStart(ntp_timer, 0);

  ntp_start();

  // Block until the sync notification or the sync window released the
  // semaphore.
  xSemaphoreTake(s_ntp_sem, pdMS_TO_TICKS(NTP_SYNC_WINDOW_MS + 500));
  if (ntp_timer)
    xTimerDelete(ntp_timer, 0);
  if (s_ntp_sem) {
    vSemaphoreDelete(s_ntp_sem);
    s_ntp_sem = NULL;
  }
  return get_epoch_ms() > 1000000000LL ? ESP_OK : ESP_ERR_TIMEOUT;
}
