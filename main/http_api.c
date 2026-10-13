/* Device HTTP API, listening on port 80. */
#include "http_api.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "environment_sensor.h"
#include "core.h"
#include "status.h"
#include "pwm_settings.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "ota_update.h"

static httpd_handle_t s_server;
static TaskHandle_t s_control_task;

/* Decode query values in-place; malformed escapes and embedded NUL are invalid. */
static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool decode_url(char *value) {
  char *src = value, *dst = value;
  while (*src) {
    if (*src == '%') {
      if (!src[1] || !src[2]) return false;
      int high = hex_digit(src[1]), low = hex_digit(src[2]);
      if (high < 0 || low < 0 || (high == 0 && low == 0)) return false;
      *dst++ = (char)((high << 4) | low);
      src += 3;
    } else {
      *dst++ = (*src == '+') ? ' ' : *src;
      ++src;
    }
  }
  *dst = '\0';
  return true;
}

static esp_err_t update_handler(httpd_req_t *req) {
  char query[OTA_URL_MAX_LEN * 3 + 8];
  char url[sizeof(query)];
  const char *override = NULL;
  httpd_resp_set_type(req, "application/json");
  size_t query_len = httpd_req_get_url_query_len(req);
  if (query_len) {
    if (query_len >= sizeof(query) ||
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
      goto invalid_url;
    esp_err_t result = httpd_query_key_value(query, "url", url, sizeof(url));
    if (result == ESP_OK) {
      if (!decode_url(url) || !url[0])
        goto invalid_url;
      override = url;
    } else if (result != ESP_ERR_NOT_FOUND) {
      goto invalid_url;
    }
  }
  esp_err_t err = ota_update_start(override);
  if (err == ESP_ERR_INVALID_ARG)
    goto invalid_url;
  if (err == ESP_ERR_INVALID_STATE) {
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "{\"error\":\"update_in_progress\"}");
  }
  if (err != ESP_OK) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "{\"error\":\"update_start_failed\"}");
  }
  httpd_resp_set_status(req, "202 Accepted");
  return httpd_resp_sendstr(req, "{\"status\":\"update_started\"}");

invalid_url:
  httpd_resp_set_status(req, "400 Bad Request");
  return httpd_resp_sendstr(req, "{\"error\":\"invalid_ota_url\"}");
}

static esp_err_t sensor_handler(httpd_req_t *req) {
  struct environment_reading r;
  environment_sensor_get(&r);
  const char *validity[] = {"normal", "warmup", "startup", "invalid"};
  char body[768], timestamp[32], error[80], reading[512];
  if (r.updated_ms)
    snprintf(timestamp, sizeof(timestamp), "%" PRId64, r.updated_ms);
  else
    snprintf(timestamp, sizeof(timestamp), "null");
  if (r.error)
    snprintf(error, sizeof(error), "\"%s\"", r.error);
  else
    snprintf(error, sizeof(error), "null");
  if (r.available) {
    snprintf(reading, sizeof(reading),
             "{\"temperature_c\":%.2f,\"humidity_rh_pct\":%.2f,"
             "\"compensation_temperature_c\":%.2f,\"compensation_humidity_rh_pct\":%.2f,"
             "\"aqi\":%u,\"tvoc_ppb\":%u,\"eco2_ppm\":%u,"
             "\"validity\":\"%s\",\"new_data\":%s}",
             r.temperature_c, r.humidity_rh_pct,
             r.compensation_temperature_c, r.compensation_humidity_rh_pct,
             (unsigned)r.aqi,
             (unsigned)r.tvoc_ppb, (unsigned)r.eco2_ppm,
             validity[r.validity & 3], r.new_data ? "true" : "false");
  } else {
    snprintf(reading, sizeof(reading), "null");
  }
  snprintf(body, sizeof(body),
           "{\"status\":\"%s\",\"last_updated_unix_ms\":%s,"
           "\"reading\":%s,\"error\":%s}",
           r.error ? "error" : "ok", timestamp, reading, error);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (r.error) httpd_resp_set_status(req, "503 Service Unavailable");
  return httpd_resp_sendstr(req, body);
}

extern const uint8_t control_page_start[] asm("_binary_control_page_html_gz_start");
extern const uint8_t control_page_end[] asm("_binary_control_page_html_gz_end");

static esp_err_t page_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, (const char *)control_page_start,
                        control_page_end - control_page_start);
}

static esp_err_t control_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  bool enabled = !s_power_saving && !s_boot_loading;
  xSemaphoreGive(s_mutex);
  if (!enabled) {
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"pwr_required\"}");
  }
  char body[96];
  snprintf(body, sizeof(body), "{\"status\":\"ok\",\"power_mode\":\"PWR\",\"control_mode\":\"%s\"}",
           nmos_control_mode_name());
  return httpd_resp_sendstr(req, body);
}

static esp_err_t control_handler(httpd_req_t *req) {
  char query[64], value[16], body[96];
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  size_t length = httpd_req_get_url_query_len(req);
  bool explicit_mode = false;
  if (length) {
    if (length >= sizeof(query) ||
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
      goto invalid_mode;
    esp_err_t result = httpd_query_key_value(query, "mode", value, sizeof(value));
    if (result == ESP_OK) {
      if (!decode_url(value)) goto invalid_mode;
      explicit_mode = true;
    } else if (result != ESP_ERR_NOT_FOUND) {
      goto invalid_mode;
    }
  }
  enum nmos_mode mode;
  esp_err_t err;
  if (!explicit_mode) {
    err = nmos_control_next_mode(&mode);
  } else {
    if (strcmp(value, "RA") == 0) mode = NMOS_RA;
    else if (strcmp(value, "ON") == 0) mode = NMOS_ON;
    else if (strcmp(value, "BL") == 0) mode = NMOS_BL;
    else if (strcmp(value, "OFF") == 0) mode = NMOS_OFF;
    else goto invalid_mode;
    err = nmos_control_set_mode(mode);
  }
  if (err == ESP_ERR_INVALID_STATE) {
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"pwr_required\"}");
  }
  if (err != ESP_OK) goto invalid_mode;
  snprintf(body, sizeof(body), "{\"status\":\"ok\",\"power_mode\":\"PWR\",\"control_mode\":\"%s\"}",
           mode == NMOS_RA ? "RA" : mode == NMOS_ON ? "ON" : mode == NMOS_BL ? "BL" : "OFF");
  return httpd_resp_sendstr(req, body);
invalid_mode:
  httpd_resp_set_status(req, "400 Bad Request");
  return httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"invalid_control_mode\"}");
}

static esp_err_t pwm_response(httpd_req_t *req) {
  struct pwm_settings settings;
  pwm_settings_get(&settings);
  char body[128];
  snprintf(body, sizeof(body), "{\"status\":\"ok\",\"frequency_hz\":%u,"
           "\"ctl1_duty_pct\":%u,\"ctl2_duty_pct\":%u}",
           (unsigned)CFG_NMOS_PWM_FREQ_HZ, settings.ctl1, settings.ctl2);
  return httpd_resp_sendstr(req, body);
}

static bool duty_parse(const char *text, unsigned *out) {
  if (!text[0]) return false;
  unsigned value = 0;
  for (const char *p = text; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    value = value * 10 + (*p - '0');
    if (value > 100) return false;
  }
  *out = value;
  return true;
}

static esp_err_t pwm_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  bool pwr = !s_power_saving && !s_boot_loading;
  xSemaphoreGive(s_mutex);
  if (!pwr) goto pwr_required;
  if (req->method == HTTP_GET) return pwm_response(req);
  char query[96], value1[8], value2[8];
  unsigned ctl1, ctl2;
  size_t length = httpd_req_get_url_query_len(req);
  if (!length || length >= sizeof(query) ||
      httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "ctl1", value1, sizeof(value1)) != ESP_OK ||
      httpd_query_key_value(query, "ctl2", value2, sizeof(value2)) != ESP_OK ||
      !decode_url(value1) || !decode_url(value2) ||
      !duty_parse(value1, &ctl1) || !duty_parse(value2, &ctl2)) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"invalid_pwm_duty\"}");
  }
  esp_err_t err = pwm_settings_save(ctl1, ctl2);
  if (err == ESP_ERR_INVALID_STATE) goto pwr_required;
  if (err != ESP_OK) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"pwm_save_failed\"}");
  }
  return pwm_response(req);
pwr_required:
  httpd_resp_set_status(req, "409 Conflict");
  return httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"pwr_required\"}");
}

static esp_err_t http_server_start(void) {
  if (s_server)
    return ESP_OK;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.task_priority = 2; /* never preempt sampling or GPIO/PWM control */
  config.stack_size = 6144; /* JSON/OTA handlers have ~1.5KB local buffers */
  config.max_open_sockets = 4;
  config.lru_purge_enable = true;
  config.recv_wait_timeout = 2;
  config.send_wait_timeout = 2;
  esp_err_t err = httpd_start(&s_server, &config);
  if (err != ESP_OK)
    return err;
  const httpd_uri_t update = {
    .uri = "/api/ota/update",
    .method = HTTP_POST,
    .handler = update_handler,
  };
  err = httpd_register_uri_handler(s_server, &update);
  if (err == ESP_OK) {
    const httpd_uri_t sensor = {
        .uri = "/api/v1/sensor",
        .method = HTTP_GET,
        .handler = sensor_handler,
    };
    err = httpd_register_uri_handler(s_server, &sensor);
  }
  if (err == ESP_OK) {
    const httpd_uri_t control = {
        .uri = "/api/v1/control",
        .method = HTTP_POST,
        .handler = control_handler,
    };
    err = httpd_register_uri_handler(s_server, &control);
  }
  if (err == ESP_OK) {
    const httpd_uri_t query = {.uri = "/api/v1/control", .method = HTTP_GET,
                              .handler = control_get_handler};
    err = httpd_register_uri_handler(s_server, &query);
  }
  if (err == ESP_OK) {
    const httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = page_handler};
    err = httpd_register_uri_handler(s_server, &page);
  }
  if (err == ESP_OK) {
    const httpd_uri_t pwm_get = {.uri = "/api/v1/pwm", .method = HTTP_GET, .handler = pwm_handler};
    err = httpd_register_uri_handler(s_server, &pwm_get);
  }
  if (err == ESP_OK) {
    const httpd_uri_t pwm_post = {.uri = "/api/v1/pwm", .method = HTTP_POST, .handler = pwm_handler};
    err = httpd_register_uri_handler(s_server, &pwm_post);
  }
  if (err != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
  }
  return err;
}

/* Own the server handle here so stop/start cannot block the sampling task. */
static void http_control_task(void *arg) {
  (void)arg;
  for (;;) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool enabled = !s_power_saving && !s_boot_loading;
    xSemaphoreGive(s_mutex);
    esp_err_t err = ESP_OK;
    if (enabled && !s_server) {
      err = http_server_start();
      if (err == ESP_OK) ESP_LOGI("http", "PWR: HTTP listening on port 80");
    } else if (!enabled && s_server) {
      err = httpd_stop(s_server);
      if (err == ESP_OK) {
        s_server = NULL;
        ESP_LOGI("http", "ECO: HTTP stopped");
      }
    }
    if (err != ESP_OK)
      ESP_LOGW("http", "server state change failed: %s", esp_err_to_name(err));
    ulTaskNotifyTake(pdTRUE, err == ESP_OK ? portMAX_DELAY : pdMS_TO_TICKS(1000));
  }
}

esp_err_t http_api_control_start(void) {
  return xTaskCreate(http_control_task, "http-control", 3072, NULL, 2, &s_control_task) == pdPASS
             ? ESP_OK : ESP_ERR_NO_MEM;
}

void http_api_notify_power_change(void) {
  if (s_control_task) xTaskNotifyGive(s_control_task);
}
