/*
 * net_task.c — the TCP task.
 *
 * Sole owner of the socket. Drives the non-blocking connect state machine,
 * reassembles and verifies the downstream control frames (set sampling
 * interval), and sends the upstream samples that sample_task hands it over the
 * length-1 queue. All of its socket + reassembly state is private to this file.
 * The socket is driven by poll() with timeouts, so the task parks in poll()
 * when idle and never holds the socket across a blocking delay.
 */
#include "net_task.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "core.h"

static const char *TAG = "power-mon";

#define CONNECT_TIMEOUT_MS 3000u /* bound on each non-blocking connect attempt */
#define STACK_NET 4096           /* socket + lwIP + downstream reassembly */
#define PRIO_NET 2               /* low: idles when the socket is quiet */

static int s_sock = -1;
static uint32_t s_last_tcp_try = 0;

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
  }
}

void net_task_create(void) {
  xTaskCreate(net_task, "net", STACK_NET, NULL, PRIO_NET, NULL);
}
