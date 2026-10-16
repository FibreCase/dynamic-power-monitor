#include "mdns_service.h"
#include "config.h"
#include "mdns.h"
#include "esp_log.h"

static bool s_started;

esp_err_t network_mdns_set_enabled(bool enabled) {
  if (!enabled) {
    if (s_started) {
      mdns_free();
      s_started=false;
    }
    return ESP_OK;
  }
  if (s_started) return ESP_OK;

  esp_err_t err=mdns_init();
  if (err!=ESP_OK) return err;
  err=mdns_hostname_set(CFG_HOSTNAME);
  if (err==ESP_OK) err=mdns_instance_name_set(CFG_HOSTNAME);
  mdns_txt_item_t txt[]={{"path","/"}};
  if (err==ESP_OK)
    err=mdns_service_add(NULL,"_http","_tcp",80,txt,1);
  if (err!=ESP_OK) {
    mdns_free();
    return err;
  }
  s_started=true;
  ESP_LOGI("mdns","HTTP available at http://%s.local/",CFG_HOSTNAME);
  return ESP_OK;
}
