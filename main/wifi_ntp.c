/* Background WiFi radio control and NTP. */
#include "wifi_ntp.h"
#include <stdatomic.h>
#include <string.h>
#include "config.h"
#include "core.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "freertos/task.h"

static atomic_bool s_enabled = ATOMIC_VAR_INIT(false);
static atomic_bool s_ready = ATOMIC_VAR_INIT(false);
static atomic_bool s_connect_pending = ATOMIC_VAR_INIT(false);
static atomic_bool s_reassociate = ATOMIC_VAR_INIT(false);
static TaskHandle_t s_worker;
static bool s_ntp_started; /* event task only */

bool wifi_is_ready(void) {
  return atomic_load(&s_enabled) && atomic_load(&s_ready);
}

static void wake_worker(void) {
  if (s_worker) xTaskNotifyGive(s_worker);
}

esp_err_t wifi_set_enabled(bool enabled) {
  bool changed = atomic_exchange(&s_enabled, enabled) != enabled;
  if (!enabled) atomic_store(&s_ready, false);
  if (changed && enabled) atomic_store(&s_reassociate, true);
  if (changed) wake_worker();
  return ESP_OK;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  bool enabled = atomic_load(&s_enabled);
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    if (enabled) { atomic_store(&s_connect_pending, true); wake_worker(); }
  } else if ((base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) ||
             (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)) {
    atomic_store(&s_ready, false);
    if (enabled && base == WIFI_EVENT) {
      const wifi_event_sta_disconnected_t *event = data;
      ESP_LOGW("wifi", "disconnected (reason=%u), retrying", event->reason);
      atomic_store(&s_connect_pending, true);
      wake_worker();
    }
    if (enabled && base == IP_EVENT) {
      atomic_store(&s_reassociate, true);
      wake_worker();
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && enabled) {
    atomic_store(&s_ready, true);
    ESP_LOGI("wifi", "connected with IP");
    if (!s_ntp_started) {
      esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
      esp_sntp_setservername(0, CFG_NTP_HOST);
      esp_sntp_init();
      s_ntp_started = true;
    } else {
      esp_sntp_restart();
    }
  }
}

static void wifi_worker(void *arg) {
  (void)arg;
  esp_netif_t *netif = esp_netif_create_default_wifi_sta();
  _Static_assert(sizeof(CFG_HOSTNAME) > 1 && sizeof(CFG_HOSTNAME) <= 33,
                 "CFG_HOSTNAME must contain 1-32 characters");
  ESP_ERROR_CHECK(esp_netif_set_hostname(netif, CFG_HOSTNAME));
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  wifi_config_t config = {0};
  strncpy((char *)config.sta.ssid, CFG_WIFI_SSID, sizeof(config.sta.ssid) - 1);
  strncpy((char *)config.sta.password, CFG_WIFI_PASS, sizeof(config.sta.password) - 1);
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
  bool started = false, tried = false;
  uint32_t last_attempt = 0;
  for (;;) {
    bool enabled = atomic_load(&s_enabled);
    if (enabled != started) {
      esp_err_t err = enabled ? esp_wifi_start() : esp_wifi_stop();
      if (err == ESP_OK) {
        started = enabled;
        if (enabled) atomic_store(&s_reassociate, false);
        ESP_LOGI("wifi", "radio %s", enabled ? "on" : "off");
      } else {
        ESP_LOGW("wifi", "radio change failed: %s", esp_err_to_name(err));
      }
    }
    enabled = atomic_load(&s_enabled);
    if (started && enabled && atomic_exchange(&s_reassociate, false)) {
      esp_wifi_disconnect();
      atomic_store(&s_connect_pending, true);
    }
    uint32_t now = now_ms();
    if (started && enabled && atomic_load(&s_connect_pending) &&
        (!tried || now - last_attempt >= 1000u)) {
      atomic_store(&s_connect_pending, false);
      esp_err_t err = esp_wifi_connect();
      tried = true;
      last_attempt = now;
      if (err != ESP_OK) {
        atomic_store(&s_connect_pending, true);
        ESP_LOGW("wifi", "connect failed: %s; retrying", esp_err_to_name(err));
      }
    }
    if (!enabled) {
      atomic_store(&s_connect_pending, false);
      atomic_store(&s_reassociate, false);
      tried = false;
    }
    bool retry = enabled != started || (enabled && atomic_load(&s_connect_pending));
    ulTaskNotifyTake(pdTRUE, retry ? pdMS_TO_TICKS(1000) : portMAX_DELAY);
  }
}

esp_err_t wifi_background_start(void) {
  ESP_ERROR_CHECK(esp_netif_init()); /* TCP/IP needed by the HTTP server */
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  return xTaskCreate(wifi_worker, "wifi-control", 4096, NULL, 2, &s_worker) == pdPASS
             ? ESP_OK : ESP_ERR_NO_MEM;
}
