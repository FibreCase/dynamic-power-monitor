/*
 * ota_update.c — over-the-air firmware update.
 *
 * On ota_update_start() this spawns a task that:
 *   1. Picks the passive OTA slot (the one we are NOT running from).
 *   2. Refuses if the running app is still awaiting first-boot confirmation
 *      (PENDING_VERIFY) — you can't stack an OTA on top of an unconfirmed one.
 *   3. GETs the new .bin over HTTP (fixed URL from config.h), checking the
 *      Content-Length fits the slot.
 *   4. Streams it into the passive slot with esp_ota_write (incremental erase),
 *      then esp_ota_end() to validate the image.
 *   5. Compares validated app-image SHA-256 against the running image; identical
 *      images are skipped even if their version strings match a changed build.
 *   6. On success, repoints the boot slot (esp_ota_set_boot_partition) and
 *      reboots. On ANY failure it cleans up and returns, leaving the running
 *      firmware untouched and the device online.
 *
 * The download runs on its own task; the running app (and the sample / display
 * / status tasks) keep running throughout — esp_ota only ever erases/writes the
 * passive slot. Flash erase/write on the single-core C3 briefly stalls the CPU
 * per 4 KB block, so the UI may hiccup for a moment but the app does not crash.
 */
#include "ota_update.h"

#include <errno.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

#define OTA_TASK_STACK 8192
#define OTA_TASK_PRIO  1
#define OTA_READ_BUF   4096
#define HTTP_TIMEOUT_MS 15000

static char s_update_url[OTA_URL_MAX_LEN];

static atomic_bool s_updating = ATOMIC_VAR_INIT(false);

static void ota_task_finish(void) {
  atomic_store(&s_updating, false);
  vTaskDelete(NULL);
}

static void ota_update_task(void *arg) {
  (void)arg;
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *target = esp_ota_get_next_update_partition(running);
  if (running == NULL || target == NULL) {
    ESP_LOGE(TAG, "could not resolve running/update partition — no OTA");
    ota_task_finish();
    return;
  }
  ESP_LOGI(TAG, "OTA: running %s @0x%" PRIx32 ", update -> %s @0x%" PRIx32,
           running->label, running->address, target->label, target->address);

  // Refuse to start an OTA while the current app still needs first-boot
  // confirmation (esp_ota_begin would also reject it with
  // ESP_ERR_OTA_ROLLBACK_INVALID_STATE; a clear log is nicer).
  esp_ota_img_states_t run_state;
  if (esp_ota_get_state_partition(running, &run_state) == ESP_OK &&
      run_state == ESP_OTA_IMG_PENDING_VERIFY) {
    ESP_LOGE(TAG, "OTA aborted: running app is still PENDING_VERIFY (confirm it "
                  "before updating)");
    ota_task_finish();
    return;
  }

  // --- Set up the HTTP client ---
  esp_http_client_config_t cfg = {
      .url = s_update_url,
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
      .crt_bundle_attach = esp_crt_bundle_attach,
#endif
      .timeout_ms = HTTP_TIMEOUT_MS,
      .buffer_size = OTA_READ_BUF,
      .method = HTTP_METHOD_GET,
  };
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == NULL) {
    ESP_LOGE(TAG, "OTA: http client init failed");
    ota_task_finish();
    return;
  }
  if (esp_http_client_open(client, 0) != ESP_OK) {
    ESP_LOGE(TAG, "OTA: http open failed (status %d)",
             esp_http_client_get_status_code(client));
    esp_http_client_cleanup(client);
    ota_task_finish();
    return;
  }
  int64_t headers = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  if (headers < 0 || status != 200) {
    ESP_LOGE(TAG, "OTA: invalid HTTP response (%d)", status);
    esp_http_client_cleanup(client);
    ota_task_finish();
    return;
  }
  int64_t content_len = esp_http_client_get_content_length(client);
  if (content_len > 0 && content_len > (int64_t)target->size) {
    ESP_LOGE(TAG, "OTA: image (%lld bytes) does not fit slot (%" PRIu32
                 " bytes) — aborting",
             (long long)content_len, (uint32_t)target->size);
    esp_http_client_cleanup(client);
    ota_task_finish();
    return;
  }

  // --- Stream into the passive slot ---
  static uint8_t buf[OTA_READ_BUF];
  esp_ota_handle_t handle;
  esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "OTA: esp_ota_begin failed (%s)", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    ota_task_finish();
    return;
  }

  size_t total = 0;
  bool fail = false;
  for (;;) {
    errno = 0;
    int n = esp_http_client_read(client, (char *)buf, OTA_READ_BUF);
    if (n < 0) {
      ESP_LOGE(TAG, "OTA: read error (%d)", n);
      fail = true;
      break;
    }
    if (n > 0) {
      err = esp_ota_write(handle, buf, (size_t)n);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_write failed (%s)", esp_err_to_name(err));
        fail = true;
        break;
      }
      total += (size_t)n;
      vTaskDelay(1); /* bound uninterrupted work even on a fast local download */
    } else { // n == 0
      if (errno == ECONNRESET || errno == ENOTCONN) {
        ESP_LOGE(TAG, "OTA: connection reset (errno=%d)", errno);
        fail = true;
        break;
      }
      if (!esp_http_client_is_complete_data_received(client))
        fail = true;
      break;
    }
  }
  // Capture transfer completeness before we free the client (cleanup invalidates
  // the handle, so is_complete_data_received() can't be called after it).
  bool complete = esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);

  if (fail) {
    esp_ota_abort(handle);
    ESP_LOGE(TAG, "OTA: aborted — device keeps running %s", running->label);
    ota_task_finish();
    return;
  }
  if (!complete || (content_len > 0 && total != (size_t)content_len)) {
    ESP_LOGE(TAG, "OTA: incomplete download (got %zu of %lld bytes) — aborting", total,
             (long long)content_len);
    esp_ota_abort(handle);
    ota_task_finish();
    return;
  }

  ESP_LOGI(TAG, "OTA: wrote %" PRIu32 " bytes, validating image...", (uint32_t)total);
  err = esp_ota_end(handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "OTA: esp_ota_end failed (%s)%s — keeping current firmware",
             esp_err_to_name(err),
             err == ESP_ERR_OTA_VALIDATE_FAILED ? " (image invalid/corrupt)" : "");
    ota_task_finish();
    return;
  }

  // Compare complete, validated image content, independent of version strings
  // and HTTP read boundaries. App hashes exclude unused partition space.
  uint8_t running_hash[32], target_hash[32];
  err = esp_partition_get_sha256(target, target_hash);
  if (err == ESP_OK)
    err = esp_partition_get_sha256(running, running_hash);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "OTA: image SHA-256 failed (%s) — keeping current firmware",
             esp_err_to_name(err));
    ota_task_finish();
    return;
  }
  if (memcmp(running_hash, target_hash, sizeof(running_hash)) == 0) {
    ESP_LOGI(TAG, "OTA: identical image SHA-256 — skipping reboot");
    ota_task_finish();
    return;
  }

  err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "OTA: set_boot_partition failed (%s) — keeping current firmware",
             esp_err_to_name(err));
    ota_task_finish();
    return;
  }

  ESP_LOGI(TAG, "OTA: update to %s complete — rebooting into new firmware",
           target->label);
  vTaskDelay(pdMS_TO_TICKS(200)); // let this log flush
  esp_restart();
  // not reached
}

esp_err_t ota_update_start(const char *url) {
  if (!url)
    url = CFG_OTA_URL;
  size_t length = strnlen(url, OTA_URL_MAX_LEN);
  const char *host = NULL;
  if (strncmp(url, "http://", 7) == 0)
    host = url + 7;
  else if (strncmp(url, "https://", 8) == 0)
    host = url + 8;
  if (!host || !*host || *host == '/' || *host == '?' || *host == '#' ||
      length >= OTA_URL_MAX_LEN)
    return ESP_ERR_INVALID_ARG;
  for (size_t i = 0; i < length; ++i)
    if ((unsigned char)url[i] <= 32 || (unsigned char)url[i] == 127)
      return ESP_ERR_INVALID_ARG;
  bool expected = false;
  if (!atomic_compare_exchange_strong(&s_updating, &expected, true))
    return ESP_ERR_INVALID_STATE;
  memcpy(s_update_url, url, length + 1);
  if (xTaskCreate(ota_update_task, "ota", OTA_TASK_STACK, NULL,
                  OTA_TASK_PRIO, NULL) != pdPASS) {
    atomic_store(&s_updating, false);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}
