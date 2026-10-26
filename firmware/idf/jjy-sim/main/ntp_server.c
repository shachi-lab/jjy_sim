/*
 * JJY-SIM - NTP Server
 *
 * Provides an NTP server on the connected STA interface.
 * - Uses SNTP synchronization status for reply generation
 * - Manages server startup, shutdown and Wi-Fi power saving
 *
 * Copyright (c) 2026 Shachi-lab
 * SPDX-License-Identifier: MIT
 *
 * Developed by Shachi-lab
 * https://shachi-lab.com
 *   ____  _                _     _     _       _
 *  / ___)| |__   __ _  ___| |__ |_|   | | __ _| |__
 *  \___ \| '_ \ / _` |/ __) '_ \ _  _ | |/ _` | '_ \
 *   ___) | | | | (_| | (__| | | | ||_|| | (_| | |_) |
 *  (____/|_| |_|\__,_|\___)_| |_|_|   |_|\__,_|_.__/
 */
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/ip_addr.h"
#include "ntp_server.h"
#include "ntp_server_packet.h"

#define SERVER_STOP BIT0
#define SERVER_DONE BIT1
#define SYNC_MAX_AGE_US (6LL * 60 * 60 * 1000000)
static const char *TAG = "ntp_server";
static EventGroupHandle_t server_events;
static portMUX_TYPE clock_lock = portMUX_INITIALIZER_UNLOCKED;
static struct timeval last_reference;
static uint32_t last_source;
static int64_t last_sync_us;
static bool have_sync;
static bool server_running;
static wifi_ps_type_t previous_power_save;
static bool restore_power_save;

static void set_running(bool running)
{
  portENTER_CRITICAL(&clock_lock);
  server_running = running;
  portEXIT_CRITICAL(&clock_lock);
}

bool ntp_server_is_running(void)
{
  portENTER_CRITICAL(&clock_lock);
  bool running = server_running;
  portEXIT_CRITICAL(&clock_lock);
  return running;
}

void ntp_server_time_synced(struct timeval *time)
{
  (void)time;
  struct timeval reference;
  gettimeofday(&reference, NULL);
  // The callback runs in the lwIP thread; server 0 is the only configured source.
  const ip_addr_t *source = esp_sntp_getserver(0);
  uint32_t ipv4 = source && IP_IS_V4(source) ? ip_2_ip4(source)->addr : 0;
  int64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&clock_lock);
  last_reference = reference;
  last_source = ipv4;
  last_sync_us = now;
  have_sync = true;
  portEXIT_CRITICAL(&clock_lock);
}

static ntp_server_clock_t clock_snapshot(void)
{
  ntp_server_clock_t clock = {0};
  int64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&clock_lock);
  clock.reference = last_reference;
  clock.source_ipv4 = last_source;
  int64_t age = now - last_sync_us;
  clock.synchronized = have_sync && age >= 0 && age <= SYNC_MAX_AGE_US;
  portEXIT_CRITICAL(&clock_lock);
  struct timeval remaining = {0};
  adjtime(NULL, &remaining);
  int64_t correction = (int64_t)remaining.tv_sec * 1000000 + remaining.tv_usec;
  if (correction < 0) correction = -correction;
  // Baseline 100 ms plus 50 ppm holdover drift and pending smooth correction.
  int64_t uncertainty = 100000 + (age > 0 ? age / 20000 : 0) + correction;
  if (uncertainty > 1500000) clock.synchronized = false;
  clock.dispersion = (uint32_t)((uncertainty > 16000000 ? 16000000 : uncertainty) * 65536 / 1000000);
  return clock;
}

static bool stopping(void)
{
  return (xEventGroupGetBits(server_events) & SERVER_STOP) != 0;
}

static void server_task(void *arg)
{
  (void)arg;
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  int fd = -1;
  uint32_t bound_ip = 0;
  int64_t next_reply_us = 0;
  while (!stopping()) {
    // Yield even under a continuous stream of malformed datagrams.
    vTaskDelay(1);
    esp_netif_ip_info_t info = {0};
    if (!sta || !esp_netif_is_netif_up(sta) ||
        esp_netif_get_ip_info(sta, &info) != ESP_OK) info.ip.addr = 0;
    if (fd >= 0 && info.ip.addr != bound_ip) {
      set_running(false);
      close(fd);
      fd = -1;
    }
    if (!info.ip.addr) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    if (fd < 0) {
      fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      struct ifreq iface = {0};
      struct timeval timeout = {.tv_usec = 250000};
      struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(123),
                                    .sin_addr.s_addr = info.ip.addr};
      if (fd < 0 || esp_netif_get_netif_impl_name(sta, iface.ifr_name) != ESP_OK ||
          setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &iface, sizeof(iface)) < 0 ||
          setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
          bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        ESP_LOGE(TAG, "Cannot bind STA UDP/123: errno=%d", errno);
        if (fd >= 0) close(fd);
        fd = -1;
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      bound_ip = info.ip.addr;
      set_running(true);
    }
    // One extra byte detects and rejects oversized datagrams without allocation.
    uint8_t request[NTP_PACKET_SIZE + 1], reply[NTP_PACKET_SIZE];
    struct sockaddr_in peer = {0};
    socklen_t peer_len = sizeof(peer);
    int length = recvfrom(fd, request, sizeof(request), 0, (struct sockaddr *)&peer, &peer_len);
    struct timeval received, transmitted;
    gettimeofday(&received, NULL);
    if (stopping()) break;
    if (length < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        set_running(false);
        close(fd);
        fd = -1;
        vTaskDelay(pdMS_TO_TICKS(250));
      }
      continue;
    }
    uint32_t host = ntohl(peer.sin_addr.s_addr);
    if (!host || host == 0xffffffff || (host >> 28) >= 14 || !peer.sin_port) continue;
    int64_t now = esp_timer_get_time();
    if (now < next_reply_us) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    ntp_server_clock_t clock = clock_snapshot();
    gettimeofday(&transmitted, NULL);
    if (!ntp_server_make_reply(request, length, &clock, &received, &transmitted, reply)) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    next_reply_us = now + 50000; // At most 20 replies/sec total; no amplification.
    sendto(fd, reply, sizeof(reply), 0, (struct sockaddr *)&peer, peer_len);
  }
  set_running(false);
  if (fd >= 0) close(fd);
  xEventGroupSetBits(server_events, SERVER_DONE);
  vTaskDelete(NULL);
}

static esp_err_t restore_wifi_power_save(void)
{
  if (!restore_power_save) return ESP_OK;
  esp_err_t err = esp_wifi_set_ps(previous_power_save);
  restore_power_save = (err != ESP_OK);
  if (err != ESP_OK) ESP_LOGE(TAG, "Cannot restore Wi-Fi power save: %s", esp_err_to_name(err));
  return err;
}

esp_err_t ntp_server_start(void)
{
  if (server_events) return ESP_OK;
  esp_err_t err = restore_wifi_power_save();
  if (err != ESP_OK) return err;
  server_events = xEventGroupCreate();
  if (!server_events) return ESP_ERR_NO_MEM;
  // An NTP server must receive promptly, without waiting for AP power-save buffering.
  err = esp_wifi_get_ps(&previous_power_save);
  if (err == ESP_OK) err = esp_wifi_set_ps(WIFI_PS_NONE);
  if (err != ESP_OK) {
    vEventGroupDelete(server_events);
    server_events = NULL;
    return err;
  }
  restore_power_save = true;
  if (xTaskCreate(server_task, "ntp_server", 3072, NULL, 2, NULL) != pdPASS) {
    restore_wifi_power_save();
    vEventGroupDelete(server_events);
    server_events = NULL;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void ntp_server_stop(void)
{
  if (!server_events) {
    restore_wifi_power_save();
    return;
  }
  xEventGroupSetBits(server_events, SERVER_STOP);
  set_running(false);
  xEventGroupWaitBits(server_events, SERVER_DONE, pdFALSE, pdTRUE, portMAX_DELAY);
  vEventGroupDelete(server_events);
  server_events = NULL;
  restore_wifi_power_save();
}
