/*
 * ota_update.c — over-the-air firmware update.
 *
 * On ota_update_start() this spawns a task that:
 *   1. Picks the passive OTA slot (the one we are NOT running from).
 *   2. Refuses if the running app is still awaiting first-boot confirmation
 *      (PENDING_VERIFY) — you can't stack an OTA on top of an unconfirmed one.
 *   3. GETs the new .bin over HTTP (host address from config.h), checking the
 *      Content-Length fits the slot and that we aren't re-flashing the same
 *      version.
 *   4. Streams it into the passive slot with esp_ota_write (incremental erase),
 *      then esp_ota_end() to validate the image.
 *   5. On success, repoints the boot slot (esp_ota_set_boot_partition) and
 *      reboots. On ANY failure it cleans up and returns, leaving the running
 *      firmware untouched and the device online.
 *
 * The download runs on its own task; the running app (and the sample / display
 * / net tasks) keep running throughout — esp_ota only ever erases/writes the
 * passive slot. Flash erase/write on the single-core C3 briefly stalls the CPU
 * per 4 KB block, so the UI may hiccup for a moment but the app does not crash.
 */
#include "ota_update.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

#define OTA_TASK_STACK 8192
#define OTA_TASK_PRIO  4
#define OTA_READ_BUF   4096
#define HTTP_TIMEOUT_MS 15000

/* Build the OTA URL from config: http://<host>:<port><path> */
static void ota_url(char *buf, size_t len) {
  snprintf(buf, len, "http://%s:%u%s", CFG_OTA_HOST_IP, (unsigned)CFG_OTA_HOST_PORT,
           CFG_OTA_URL_PATH);
}

static void ota_update_task(void *arg) {
  (void)arg;
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *target = esp_ota_get_next_update_partition(running);
  if (running == NULL || target == NULL) {
    ESP_LOGE(TAG, "could not resolve running/update partition — no OTA");
    vTaskDelete(NULL);
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
    vTaskDelete(NULL);
    return;
  }

  // --- Set up the HTTP client ---
  char url[128];
  ota_url(url, sizeof(url));
  esp_http_client_config_t cfg = {
      .url = url,
      .timeout_ms = HTTP_TIMEOUT_MS,
      .buffer_size = OTA_READ_BUF,
      .method = HTTP_METHOD_GET,
  };
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == NULL) {
    ESP_LOGE(TAG, "OTA: http client init failed");
    vTaskDelete(NULL);
    return;
  }
  if (esp_http_client_open(client, 0) != ESP_OK) {
    ESP_LOGE(TAG, "OTA: http open failed (status %d)",
             esp_http_client_get_status_code(client));
    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
    return;
  }
  esp_http_client_fetch_headers(client);
  int64_t content_len = esp_http_client_get_content_length(client);
  if (content_len > 0 && content_len > (int64_t)target->size) {
    ESP_LOGE(TAG, "OTA: image (%lld bytes) does not fit slot (%" PRIu32
                 " bytes) — aborting",
             (long long)content_len, (uint32_t)target->size);
    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
    return;
  }

  // --- Stream into the passive slot ---
  static uint8_t buf[OTA_READ_BUF];
  esp_ota_handle_t handle;
  esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "OTA: esp_ota_begin failed (%s)", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
    return;
  }

  size_t total = 0;
  bool version_checked = false;
  bool fail = false;
  for (;;) {
    int n = esp_http_client_read(client, (char *)buf, OTA_READ_BUF);
    if (n < 0) {
      ESP_LOGE(TAG, "OTA: read error (%d)", n);
      fail = true;
      break;
    }
    if (n > 0) {
      // On the first chunk, sanity-check the incoming image's version so we
      // don't "update" to the build we're already running.
      if (!version_checked &&
          n > (int)(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
                   sizeof(esp_app_desc_t))) {
        esp_app_desc_t new_info, run_info;
        memcpy(&new_info,
               &buf[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)],
               sizeof(esp_app_desc_t));
        if (esp_ota_get_partition_description(running, &run_info) == ESP_OK) {
          ESP_LOGI(TAG, "OTA: running v%s -> new v%s", run_info.version,
                   new_info.version);
          if (memcmp(new_info.version, run_info.version, sizeof(new_info.version)) ==
              0) {
            ESP_LOGW(TAG, "OTA: new image is the same version as running — no-op, "
                          "keeping current firmware");
            fail = true;
            break;
          }
        }
        version_checked = true;
      }
      err = esp_ota_write(handle, buf, (size_t)n);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_write failed (%s)", esp_err_to_name(err));
        fail = true;
        break;
      }
      total += (size_t)n;
    } else { // n == 0
      if (errno == ECONNRESET || errno == ENOTCONN) {
        ESP_LOGE(TAG, "OTA: connection reset (errno=%d)", errno);
        fail = true;
        break;
      }
      if (esp_http_client_is_complete_data_received(client)) {
        break; // clean EOF
      }
    }
  }
  // Capture transfer completeness before we free the client (cleanup invalidates
  // the handle, so is_complete_data_received() can't be called after it).
  bool complete = esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);

  if (fail) {
    esp_ota_abort(handle);
    ESP_LOGE(TAG, "OTA: aborted — device keeps running %s", running->label);
    vTaskDelete(NULL);
    return;
  }
  if (!complete || (content_len > 0 && total != (size_t)content_len)) {
    ESP_LOGE(TAG, "OTA: incomplete download (got %zu of %lld bytes) — aborting", total,
             (long long)content_len);
    esp_ota_abort(handle);
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "OTA: wrote %" PRIu32 " bytes, validating image...", (uint32_t)total);
  err = esp_ota_end(handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "OTA: esp_ota_end failed (%s)%s — keeping current firmware",
             esp_err_to_name(err),
             err == ESP_ERR_OTA_VALIDATE_FAILED ? " (image invalid/corrupt)" : "");
    vTaskDelete(NULL);
    return;
  }

  err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "OTA: set_boot_partition failed (%s) — keeping current firmware",
             esp_err_to_name(err));
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "OTA: update to %s complete — rebooting into new firmware",
           target->label);
  vTaskDelay(pdMS_TO_TICKS(200)); // let this log flush
  esp_restart();
  // not reached
}

void ota_update_start(void) {
  ESP_LOGI(TAG, "OTA: starting update task");
  xTaskCreate(ota_update_task, "ota", OTA_TASK_STACK, NULL, OTA_TASK_PRIO, NULL);
}
