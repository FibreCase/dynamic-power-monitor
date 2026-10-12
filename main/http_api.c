/* Device HTTP API, listening on port 80. */
#include "http_api.h"
#include <string.h>
#include "esp_http_server.h"
#include "ota_update.h"

static httpd_handle_t s_server;

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

esp_err_t http_api_start(void) {
  if (s_server)
    return ESP_OK;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  esp_err_t err = httpd_start(&s_server, &config);
  if (err != ESP_OK)
    return err;
  const httpd_uri_t update = {
    .uri = "/api/ota/update",
    .method = HTTP_POST,
    .handler = update_handler,
  };
  err = httpd_register_uri_handler(s_server, &update);
  if (err != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
  }
  return err;
}
