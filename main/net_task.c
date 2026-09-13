/*
 * net_task.c — the TCP task.
 *
 * Sole owner of the socket. Drives the non-blocking connect state machine,
 * reassembles and verifies the downstream control frames (set sampling
 * interval), sends the upstream samples that sample_task hands it over the
 * length-1 queue, and sends a one-time device-info frame (running firmware
 * version + active OTA slot) each time it (re)connects. All of its socket +
 * reassembly state is private to this file. The socket is driven by poll() with
 * timeouts, so the task parks in poll() when idle and never holds the socket
 * across a blocking delay.
 */
#include "net_task.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "core.h"
#include "ota_update.h"

static const char *TAG = "power-mon";

#define CONNECT_TIMEOUT_MS 3000u /* bound on each non-blocking connect attempt */
#define STACK_NET 4096           /* socket + lwIP + downstream reassembly */
#define PRIO_NET 2               /* low: idles when the socket is quiet */

static int s_sock = -1;
static uint32_t s_last_tcp_try = 0;
static bool s_info_sent = false; /* device-info frame sent for the current conn */

/* Downstream reassembly (handles sticky / partial packets). */
static uint8_t s_rx[64];
static uint8_t s_rxn = 0;

/* --- checksum helper --- */
static uint16_t byte_sum(const uint8_t *p, int n) {
  uint32_t s = 0;
  for (int i = 0; i < n; i++)
    s += p[i];
  return (uint16_t)(s & 0xFFFF);
}

/* --- connection --- */
static void net_close(void) {
  if (s_sock >= 0)
    close(s_sock);
  s_sock = -1;
  s_tcp_state = T_DISC;
  s_rxn = 0; /* discard any partially-received downstream frame */
  s_info_sent = false; /* re-send device info on the next (re)connect */
}

/* Drive a (re)connect to completion. Returns true and leaves
 * s_tcp_state==T_OK once connected; otherwise retries no more often than
 * TCP_RETRY_MS. net_task is the only caller, so the socket + state here need
 * no lock. */
static bool ensure_connected(void) {
  if (s_tcp_state == T_OK)
    return true;

  uint32_t now = now_ms();
  if (now - s_last_tcp_try < TCP_RETRY_MS)
    return false;
  s_last_tcp_try = now;

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    ESP_LOGE(TAG, "socket() failed");
    return false;
  }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); /* no Nagle */
  int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);

  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_port = htons(CFG_HOST_PORT);
  inet_pton(AF_INET, CFG_HOST_IP, &a.sin_addr);

  int cr = connect(fd, (struct sockaddr *)&a, sizeof(a));
  if (cr == 0) {
    s_sock = fd;
    s_tcp_state = T_OK;
    ESP_LOGI(TAG, "TCP connected");
    return true;
  }
  if (errno == EINPROGRESS) {
    struct pollfd p = {.fd = fd, .events = POLLOUT};
    if (poll(&p, 1, (int)CONNECT_TIMEOUT_MS) > 0) {
      int err = 0;
      socklen_t l = sizeof(err);
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
      if (err == 0) {
        s_sock = fd;
        s_tcp_state = T_OK;
        ESP_LOGI(TAG, "TCP connected");
        return true;
      }
      ESP_LOGW(TAG, "TCP connect failed: %s", strerror(err));
    } else {
      ESP_LOGW(TAG, "TCP connect timeout");
    }
  } else {
    ESP_LOGE(TAG, "connect() failed: %s", strerror(errno));
  }
  close(fd);
  return false;
}

/* --- upstream: build + send a 20-byte sample from the given reading --- */
static void send_sample(const struct reading *r) {
  if (s_sock < 0 || s_tcp_state != T_OK)
    return;

  int64_t ts = r->ts; /* captured at read time by sample_task */
  uint8_t buf[20];
  buf[0] = 0xAA;
  buf[1] = 0x55;
  for (int i = 0; i < 8; i++)
    buf[2 + i] = (uint8_t)((uint64_t)ts >> (8 * i)); /* u64 LE */

  union {
    float f;
    uint8_t b[4];
  } v = {.f = r->v};
  union {
    float f;
    uint8_t b[4];
  } c = {.f = r->i};
  for (int i = 0; i < 4; i++) {
    buf[10 + i] = v.b[i];
    buf[14 + i] = c.b[i];
  }

  uint16_t ck = byte_sum(buf, 18);
  buf[18] = (uint8_t)(ck & 0xFF);
  buf[19] = (uint8_t)(ck >> 8);

  (void)send(s_sock, buf, sizeof(buf), 0);
}

/* --- upstream: build + send a 25-byte OCP event from the given reading ---
 * Sent when status_task publishes an overcurrent event (INA226 ALERT edge).
 *
 * Frame: AA 54 | u8 type | u64 timestamp_ms | f32 voltage(V) | f32 current(mA)
 *        | f32 power(mW) | u16 checksum (over the first 23 bytes) */
static void send_event(const struct event *ev) {
  if (s_sock < 0 || s_tcp_state != T_OK)
    return;

  uint8_t buf[25];
  buf[0] = 0xAA;
  buf[1] = 0x54;
  buf[2] = ev->type;
  for (int i = 0; i < 8; i++)
    buf[3 + i] = (uint8_t)((uint64_t)ev->ts >> (8 * i)); /* u64 LE */

  union {
    float f;
    uint8_t b[4];
  } v = {.f = ev->v};
  union {
    float f;
    uint8_t b[4];
  } c = {.f = ev->i};
  union {
    float f;
    uint8_t b[4];
  } p = {.f = ev->p};
  for (int i = 0; i < 4; i++) {
    buf[11 + i] = v.b[i];
    buf[15 + i] = c.b[i];
    buf[19 + i] = p.b[i];
  }

  uint16_t ck = byte_sum(buf, 23);
  buf[23] = (uint8_t)(ck & 0xFF);
  buf[24] = (uint8_t)(ck >> 8);

  (void)send(s_sock, buf, sizeof(buf), 0);
}

/* --- upstream: build + send the one-time device-info frame (37 bytes) ---
 * Reports the running firmware's version string and which OTA slot it runs
 * from, so the dashboard can show the current firmware + active slot. Sent once
 * per (re)connect (see the net_task loop), before the sample stream.
 *
 * Frame: AA 53 | 32s version (NUL-padded) | u8 slot | u16 checksum
 *   slot: 1 = ota_0, 2 = ota_1, 0 = unknown (normalized from the partition
 *         subtype, whose raw values are 0x10/0x11).
 *   checksum = sum of the first 35 bytes & 0xFFFF. */
static void send_device_info(void) {
  if (s_sock < 0 || s_tcp_state != T_OK)
    return;

  uint8_t slot = 0;
  const esp_partition_t *run = esp_ota_get_running_partition();
  if (run != NULL) {
    if (run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0)
      slot = 1;
    else if (run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1)
      slot = 2;
  }

  uint8_t buf[37];
  buf[0] = 0xAA;
  buf[1] = 0x53;
  const esp_app_desc_t *desc = esp_app_get_description();
  memcpy(&buf[2], desc->version, 32); /* version is a 32-byte NUL-padded field */
  buf[34] = slot;

  uint16_t ck = byte_sum(buf, 35);
  buf[35] = (uint8_t)(ck & 0xFF);
  buf[36] = (uint8_t)(ck >> 8);

  (void)send(s_sock, buf, sizeof(buf), 0);
  ESP_LOGI(TAG, "device info sent (v%s, slot=%u)", desc->version, (unsigned)slot);
}

/* --- downstream: parse host -> ESP32 control frames (0xBB 0x66) --- */
static void process_downstream(void) {
  for (;;) {
    if (s_rxn < 8)
      break;
    if (s_rx[0] != 0xBB || s_rx[1] != 0x66) { /* resync past bad header */
      memmove(s_rx, s_rx + 1, s_rxn - 1);
      s_rxn--;
      continue;
    }
    uint16_t calc = byte_sum(s_rx, 6);
    uint16_t got = s_rx[6] | (s_rx[7] << 8);
    if (got != calc) { /* checksum fail */
      memmove(s_rx, s_rx + 1, s_rxn - 1);
      s_rxn--;
      continue;
    }
    uint8_t cmd = s_rx[2];
    uint8_t len = s_rx[3];
    uint16_t interval = s_rx[4] | (s_rx[5] << 8);
    if (cmd == 0x01 && len == 0x02)
      set_sampling_interval(interval);
    else if (cmd == 0x02) { /* start OTA: download .bin, validate, reboot */
      ESP_LOGI(TAG, "OTA command received (host -> start update)");
      ota_update_start();
    } else {
      ESP_LOGW(TAG, "unknown downstream cmd=0x%02X len=%u (ignored)", cmd, (unsigned)len);
    }
    memmove(s_rx, s_rx + 8, s_rxn - 8);
    s_rxn -= 8;
  }
}

/* --- the task --- */
static void net_task(void *arg) {
  (void)arg;
  for (;;) {
    if (!ensure_connected()) {
      vTaskDelay(pdMS_TO_TICKS(50)); /* back off between retries */
      continue;
    }

    // Once per (re)connect, tell the host which firmware we're running and
    // from which OTA slot (net_close() resets s_info_sent on every disconnect).
    if (!s_info_sent) {
      send_device_info();
      s_info_sent = true;
    }

    struct pollfd p = {.fd = s_sock, .events = POLLIN};
    int r = poll(&p, 1, 100);
    if (r < 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
      ESP_LOGW(TAG, "TCP disconnected");
      net_close();
      continue;
    }
    if (r > 0 && (p.revents & POLLIN)) {
      int n = recv(s_sock, &s_rx[s_rxn], sizeof(s_rx) - s_rxn, 0);
      if (n > 0) {
        s_rxn = (uint8_t)(s_rxn + n);
        process_downstream();
      } else if (n == 0) { /* peer closed */
        net_close();
        continue;
      }
    }

    // Send the newest sample, if sample_task has produced one. The length-1
    // queue carries at most the latest reading, so at most one fresh sample is
    // sent per pass and nothing accumulates if the host stalls.
    struct reading m;
    if (xQueueReceive(s_sample_q, &m, 0) == pdTRUE)
      send_sample(&m);

    // Send any discrete events (overcurrent ALERTs) status_task has queued.
    struct event ev;
    while (xQueueReceive(s_event_q, &ev, 0) == pdTRUE)
      send_event(&ev);
  }
}

void net_task_create(void) {
  xTaskCreate(net_task, "net", STACK_NET, NULL, PRIO_NET, NULL);
}
