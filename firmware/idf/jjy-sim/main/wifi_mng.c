/*
* JJY-SIM
*
* Wi-Fi management module
* - Handles connection and reconnection
* - Provides status for UI
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
// Keep portal diagnostics visible with the project-wide WARN setting.
#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"
#include "dhcpserver/dhcpserver.h"

#include "jjy-sim.h"
#include "settings.h"
#include "wifi_mng.h"
#include "wifi_mng_def.h"
#include "ota_update.h"

static const char *TAG __attribute__((unused)) = "wifi_mng";

#define MAX_AP_RECORDS          20
#define PORTAL_HTTP_MAX_CLIENTS  4
#define DNS_PORT                53
#define DNS_MAX_LEN             512

#define SEND_BUF_SIZE           512

#define STA_CONNECT_TIMEOUT_MS  1000

#define POST_NAME_SSID		  "ssid"
#define POST_NAME_SSID_MANUAL  "ssid_manual"
#define WIFI_SSID_MAX_BYTES 32
#define POST_NAME_PASS		  "pass"
#define POST_NAME_OUTPUT     "output"
#define POST_NAME_PPS_TX_ONLY "pps_tx_only"
#define POST_NAME_AP_PASS     "ap_pass"
#define POST_NAME_AP_WINDOW   "ap_window_min"
#define POST_NAME_SCREENSAVER "screen_min"
#define POST_NAME_SCREEN_TRIGGER "screen_trigger"
#define POST_NAME_SCREEN_OFF  "screen_off"
#define POST_NAME_BAND		  "band"
#define POST_NAME_TZ		    "tz"
#define POST_NAME_DST		    "dst"
#define POST_NAME_HOURLY	  "hourly"
#define POST_NAME_RANGE		  "range"
#define POST_NAME_DISP		  "disp"
#define POST_NAME_BRIGHT	  "bright"
#define POST_NAME_NIGHT		  "night"
#define POST_NAME_NTP_MODE	"ntpmode"
#define POST_NAME_NTP_SERVER "ntp_server"
#define POST_NAME_NTP_URL	  "ntpurl"
#define POST_NAME_OFFSET	  "ofs_tm"
#define POST_NAME_IP_MODE   "ipmode"
#define POST_NAME_IP_ADDR   "ipaddr"
#define POST_NAME_IP_MASK   "ipmask"
#define POST_NAME_IP_GW     "ipgw"
#define POST_NAME_IP_DNS    "ipdns"

static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static httpd_handle_t s_httpd = NULL;
static esp_ip4_addr_t s_ap_ip;
static wifi_ap_record_t s_scan_records[MAX_AP_RECORDS];
static uint16_t s_scan_count = 0;
static char ip_str[16] = {0};
static bool setup_mode = false;
static bool sta_setup_active = false;
static bool update_network = false;
static bool setup_recovery = false;
static bool wifi_driver_started = false;
static bool reboot_req = false;
static wifi_status_t wifi_status = WIFI_STATUS_UNKNOWN;

// The deadline and request are serialized so an expiry cannot tear down an accepted request.
typedef enum { PORTAL_CLOSED, PORTAL_WINDOW, PORTAL_REQUESTED, PORTAL_ACTIVE } portal_state_t;
static portal_state_t portal_state = PORTAL_CLOSED;
static int64_t portal_deadline_us;
static int64_t sta_retry_due_us;
static bool sta_pause_ap_pending;
#define AP_SCAN_HOME_DWELL_MS 150
#define AP_SCAN_MIN_DWELL_MS 120
#define AP_SCAN_MAX_DWELL_MS 300
#define STA_RETRY_DELAY_US (5 * 1000000LL)
static portMUX_TYPE portal_lock = portMUX_INITIALIZER_UNLOCKED;
#define PORTAL_READY_BIT BIT1
#define UPDATE_STA_READY_BIT BIT4
#define DNS_RUN_BIT BIT2
#define DNS_DONE_BIT BIT3

static portal_state_t portal_get_state(void)
{
  portENTER_CRITICAL(&portal_lock);
  portal_state_t state = portal_state;
  portEXIT_CRITICAL(&portal_lock);
  return state;
}

static bool portal_request_setup(bool via_sta)
{
  int64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&portal_lock);
  if (portal_state == PORTAL_WINDOW && now < portal_deadline_us &&
      (!via_sta || s_settings.setup_sta_enabled)) {
    sta_setup_active = via_sta;
    portal_state = PORTAL_REQUESTED;
  }
  bool accepted = (portal_state == PORTAL_REQUESTED || portal_state == PORTAL_ACTIVE) &&
                  (!via_sta || (sta_setup_active && s_settings.setup_sta_enabled));
  portEXIT_CRITICAL(&portal_lock);
  return accepted;
}

// Run session cleanup on the HTTP task, never block the Wi-Fi event callback.
static void portal_close_disconnected_sessions(void *arg)
{
  httpd_handle_t server = (httpd_handle_t)arg;
  if (server == NULL || server != s_httpd || sta_setup_active) return;
  wifi_sta_list_t clients = {0};
  // A station may already have rejoined before this work item runs.
  if (esp_wifi_ap_get_sta_list(&clients) != ESP_OK || clients.num != 0) return;

  int fds[PORTAL_HTTP_MAX_CLIENTS];
  size_t count = PORTAL_HTTP_MAX_CLIENTS;
  if (httpd_get_client_list(server, &count, fds) != ESP_OK) return;
  for (size_t i = 0; i < count; i++) {
    esp_err_t err = httpd_sess_trigger_close(server, fds[i]);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND)
      ESP_LOGW(TAG, "Cannot release disconnected HTTP session: %s", esp_err_to_name(err));
  }
}

void disp_brightness(int hour);

static esp_err_t httpd_resp_send_printf(httpd_req_t *req, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void reboot_req_handler(void)
{
  reboot_req = true;
  vTaskDelay(pdMS_TO_TICKS(5000));
  esp_restart();
}

static void html_begin(httpd_req_t *req)
{
  httpd_resp_set_type(req, "text/html; charset=utf-8");
}

static bool portal_is_interface_request(httpd_req_t *req, esp_ip4_addr_t address);

static const char *portal_root_location(httpd_req_t *req)
{
  return portal_is_interface_request(req, s_ap_ip) ? "http://192.168.4.1/" : "/";
}

static void redirect_root(httpd_req_t *req)
{
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", portal_root_location(req));
  httpd_resp_send(req, "Redirect", HTTPD_RESP_USE_STRLEN);
}

static int hex_value(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool url_decode(char *dst, const char *src, size_t dst_len)
{
  size_t di = 0;
  for (size_t i = 0; src[i] != '\0'; i++) {
    if (di + 1 >= dst_len) return false;
    if (src[i] == '+') {
      dst[di++] = ' ';
    } else if (src[i] == '%') {
      if (!src[i + 1] || !src[i + 2]) return false;
      int hi = hex_value(src[i + 1]), lo = hex_value(src[i + 2]);
      if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) return false;
      dst[di++] = (char)((hi << 4) | lo);
      i += 2;
    } else {
      dst[di++] = src[i];
    }
  }
  dst[di] = '\0';
  return true;
}

static bool form_get_value(const char *body, const char *key, char *out, size_t out_len)
{
  out[0] = '\0';
  size_t key_len = strlen(key);
  for (const char *p = body; *p;) {
    const char *e = strchr(p, '&');
    size_t field_len = e ? (size_t)(e - p) : strlen(p);
    if (field_len >= key_len && strncmp(p, key, key_len) == 0) {
      size_t len = field_len - key_len;
      char temp[512];
      if (len >= sizeof(temp)) return false;
      memcpy(temp, p + key_len, len);
      temp[len] = '\0';
      if (url_decode(out, temp, out_len)) return true;
      out[0] = '\0';
      return false;
    }
    if (!e) break;
    p = e + 1;
  }
  return false;
}

static bool form_get_ssid(const char *body, char *out, size_t out_len)
{
  if (!form_get_value(body, POST_NAME_SSID "=", out, out_len)) return false;
  // Empty selection means manual entry; a nonempty SSID always comes from the list.
  if (out[0] == '\0' &&
      !form_get_value(body, POST_NAME_SSID_MANUAL "=", out, out_len)) return false;
  size_t len = strlen(out);
  return len > 0 && len <= WIFI_SSID_MAX_BYTES;
}

// Four decimal octets only: no abbreviated, hexadecimal or trailing input.
static bool parse_ipv4(const char *text, esp_ip4_addr_t *out)
{
  uint32_t addr = 0;
  for (int i = 0; i < 4; i++) {
    unsigned octet = 0, digits = 0;
    while (*text >= '0' && *text <= '9') {
      octet = octet * 10 + (*text++ - '0');
      if (++digits > 3 || octet > 255) return false;
    }
    if (digits == 0) return false;
    addr = (addr << 8) | octet;
    if (i < 3) {
      if (*text++ != '.') return false;
    } else if (*text != '\0') return false;
  }
  out->addr = esp_netif_htonl(addr);
  return true;
}

static bool ipv4_is_unicast(esp_ip4_addr_t ip)
{
  uint32_t first = esp_netif_htonl(ip.addr) >> 24;
  return first != 0 && first != 127 && first < 224;
}

static const char *static_ip_info(const app_settings_t *cfg,
                                esp_netif_ip_info_t *info, esp_netif_dns_info_t *dns)
{
  memset(info, 0, sizeof(*info));
  memset(dns, 0, sizeof(*dns));
  dns->ip.type = ESP_IPADDR_TYPE_V4;
  if (!parse_ipv4(cfg->wifi_ip, &info->ip) || !ipv4_is_unicast(info->ip))
    return "IP address must be a valid IPv4 unicast address.";
  if (!parse_ipv4(cfg->wifi_netmask, &info->netmask))
    return "Subnet mask must be a dotted-decimal IPv4 mask.";
  uint32_t mask = esp_netif_htonl(info->netmask.addr), host = ~mask;
  if (mask == 0 || host < 3 || (host & (host + 1)) != 0)
    return "Subnet mask must be contiguous (/1 to /30).";
  uint32_t ip = esp_netif_htonl(info->ip.addr);
  if ((ip & host) == 0 || (ip & host) == host)
    return "IP address cannot be the network or broadcast address.";
  if (!parse_ipv4(cfg->wifi_gateway, &info->gw) || !ipv4_is_unicast(info->gw))
    return "Gateway must be a valid IPv4 unicast address.";
  uint32_t gw = esp_netif_htonl(info->gw.addr);
  if ((ip & mask) != (gw & mask) || gw == ip || (gw & host) == 0 || (gw & host) == host)
    return "Gateway must be another host in the same subnet.";
  if (!parse_ipv4(cfg->wifi_dns, &dns->ip.u_addr.ip4) || !ipv4_is_unicast(dns->ip.u_addr.ip4))
    return "DNS server must be a valid IPv4 unicast address.";
  return NULL;
}

static esp_err_t configure_sta_ip(void)
{
  // Settings are applied before start; changing IP mode uses the existing reboot path.
  if (!s_settings.wifi_static_ip) return ESP_OK;
  esp_netif_ip_info_t info;
  esp_netif_dns_info_t dns;
  const char *error = static_ip_info(&s_settings, &info, &dns);
  if (error) {
    ESP_LOGE(TAG, "%s", error);
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t err = esp_netif_dhcpc_stop(s_sta_netif);
  if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return err;
  err = esp_netif_set_ip_info(s_sta_netif, &info);
  if (err != ESP_OK) return err;
  // set_ip_info clears main/backup DNS, so set DNS afterwards.
  return esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
}

static void make_ap_ssid(char *out, size_t out_len)
{
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(out, out_len, WIFI_AP_SSID_PREFIX "%02X%02X%02X", (int)jjysim_board_get(), mac[3], mac[4], mac[5]);
}

const char *wifi_get_ap_ssid(void)
{
  static char ssid[32];
  make_ap_ssid(ssid, sizeof(ssid));
  return ssid;
}

bool wifi_is_setup_window_active(void)
{
  return portal_get_state() == PORTAL_WINDOW;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
  static bool is_started = false;

  if (event_base == WIFI_EVENT) {
    switch (event_id) {
    case WIFI_EVENT_STA_START:
      if (setup_mode) {
        xEventGroupSetBits(s_wifi_event_group, UPDATE_STA_READY_BIT);
        return;
      }
      if (setup_mode || portal_get_state() == PORTAL_REQUESTED) return;   
      // esp-netif clears IP/DHCP state on STA_STOP, including static IP settings.
      // The default STA_START handler has recreated the netif before this callback.
      if (configure_sta_ip() != ESP_OK) {
        ESP_LOGE(TAG, "STA IP reconfiguration failed");
      }
      is_started = true;
      wifi_status = WIFI_STATUS_CONNECTING;
      ESP_LOGI(TAG, "STA interface started");
      break;

    case WIFI_EVENT_STA_DISCONNECTED: {
      xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
      if (update_network) {
        return;
      }
      if (setup_mode && sta_setup_active) {
        // Keep a STA-origin setup session reachable after a transient link loss.
        esp_wifi_connect();
        return;
      }
      if (setup_mode || portal_get_state() == PORTAL_REQUESTED) return;
      wifi_event_sta_disconnected_t *disconnected = event_data;
      ESP_LOGW(TAG, "STA disconnected: reason=%u; retry in 5 seconds", disconnected->reason);
      xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
      // Leave time on the AP channel between connection scans.
      portENTER_CRITICAL(&portal_lock);
      sta_retry_due_us = esp_timer_get_time() + STA_RETRY_DELAY_US;
      sta_pause_ap_pending = portal_state == PORTAL_WINDOW;
      portEXIT_CRITICAL(&portal_lock);
      if (is_started == false) wifi_status = WIFI_STATUS_DISCONNECTED;
      break;
    }

    case WIFI_EVENT_AP_STACONNECTED: {
      wifi_event_ap_staconnected_t *client = event_data;
      ESP_LOGI(TAG, "AP client connected: aid=%u", client->aid);
      if (!setup_mode && portal_request_setup(false) && !sta_setup_active) {
        // Stop connection scans now; the main task stops JJY and updates the OLED.
        portENTER_CRITICAL(&portal_lock);
        sta_retry_due_us = 0;
        portEXIT_CRITICAL(&portal_lock);
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (err != ESP_OK) {
          ESP_LOGW(TAG, "STA stop failed: %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "AP client requested setup; STA connection attempts stopped");
      }
      break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
      wifi_event_ap_stadisconnected_t *client = event_data;
      ESP_LOGW(TAG, "AP client disconnected: aid=%u reason=%u", client->aid, client->reason);
      if (s_httpd != NULL && portal_get_state() == PORTAL_ACTIVE) {
        esp_err_t err = httpd_queue_work(s_httpd, portal_close_disconnected_sessions, s_httpd);
        if (err != ESP_OK) ESP_LOGW(TAG, "Cannot queue HTTP disconnect cleanup: %s", esp_err_to_name(err));
      }
      break;
    }
    case WIFI_EVENT_AP_START:
      // AP publication alone must not suppress STA reconnect or NTP.
      ESP_LOGI(TAG, "SoftAP start");
      break;

    default:
      break;
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    if (update_network || (setup_mode && sta_setup_active)) {
      ip_event_got_ip_t *event = event_data;
      esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
      xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
      return;
    }
    if (setup_mode || portal_get_state() == PORTAL_REQUESTED) return;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
    portENTER_CRITICAL(&portal_lock);
    sta_retry_due_us = 0;
    sta_pause_ap_pending = false;
    portEXIT_CRITICAL(&portal_lock);
    is_started = false;
    wifi_status = WIFI_STATUS_CONNECTED;
    ESP_LOGI(TAG, "STA got IP");
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED) {
    ip_event_ap_staipassigned_t *client = event_data;
    ESP_LOGI(TAG, "AP client DHCP address: " IPSTR, IP2STR(&client->ip));
  }
}

void wifi_init_common(void)
{
  esp_log_level_set(TAG, ESP_LOG_INFO);
  // Include driver authentication/association diagnostics during AP coexistence testing.
  esp_log_level_set("wifi", ESP_LOG_INFO);
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  s_wifi_event_group = xEventGroupCreate();

  if (s_sta_netif == NULL) {
    s_sta_netif = esp_netif_create_default_wifi_sta();
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(
    esp_event_handler_instance_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &wifi_event_handler,
                                        NULL,
                                        NULL));

  ESP_ERROR_CHECK(
    esp_event_handler_instance_register(IP_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &wifi_event_handler,
                                        NULL,
                                        NULL));
}

static esp_err_t start_wifi_scan(void)
{
  wifi_scan_config_t scan_config = {
    .ssid = NULL,
    .bssid = NULL,
    .channel = 0,
    .show_hidden = false,
    .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    .scan_time.active = { .min = AP_SCAN_MIN_DWELL_MS, .max = AP_SCAN_MAX_DWELL_MS },
    .home_chan_dwell_time = AP_SCAN_HOME_DWELL_MS,
  };

  // SSID scans need STA enabled; return to AP-only even if the scan fails.
  wifi_mode_t original_mode;
  esp_err_t err = esp_wifi_get_mode(&original_mode);
  if (err != ESP_OK) return err;
  bool restore_ap = original_mode == WIFI_MODE_AP;
  if (restore_ap) {
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;
  }
  ESP_LOGI(TAG, "Starting Wi-Fi scan...");
  err = esp_wifi_scan_start(&scan_config, true);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "scan start failed: %s", esp_err_to_name(err));
    goto scan_done;
  }

  s_scan_count = MAX_AP_RECORDS;
  err = esp_wifi_scan_get_ap_records(&s_scan_count, s_scan_records);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "get ap records failed: %s", esp_err_to_name(err));
    goto scan_done;
  }

  ESP_LOGI(TAG, "Scan done: %u APs", s_scan_count);
scan_done:
  if (restore_ap) {
    esp_err_t restore_err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (restore_err != ESP_OK) {
      ESP_LOGE(TAG, "AP-only restore failed: %s", esp_err_to_name(restore_err));
      if (err == ESP_OK) err = restore_err;
    }
  }
  return err;
}

#if 0
static void wifi_stop_all(void)
{
  esp_wifi_stop();
}
#endif

void wifi_prepare_sta(const char *ssid, const char *pass)
{
  if (s_sta_netif == NULL) {
    s_sta_netif = esp_netif_create_default_wifi_sta();
  }
  setup_mode = false;

  wifi_config_t wifi_config = {0};
  // The zero-initialized driver field accepts all 32 SSID bytes without a terminator.
  memcpy(wifi_config.sta.ssid, ssid, strnlen(ssid, sizeof(wifi_config.sta.ssid)));
  strlcpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
  wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  esp_err_t err = configure_sta_ip();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "STA IP configuration failed: %s", esp_err_to_name(err));
    wifi_status = WIFI_STATUS_DISCONNECTED;
    return; // Existing connection timeout returns to the setup portal.
  }
  ESP_LOGI(TAG, "STA settings prepared for SSID=%s", ssid);
}

static void wifi_start_apsta_for_setup(bool sta_running)
{
  if (setup_mode) wifi_status = WIFI_STATUS_UNKNOWN;

  if (s_ap_netif == NULL) {
    s_ap_netif = esp_netif_create_default_wifi_ap();
  }
  if (s_sta_netif == NULL) {
    s_sta_netif = esp_netif_create_default_wifi_sta();
  }

  char ap_ssid[32];
  make_ap_ssid(ap_ssid, sizeof(ap_ssid));

  wifi_config_t ap_config = {0};
  strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
  ap_config.ap.ssid_len = strlen(ap_ssid);
  ap_config.ap.channel = 1;
  ap_config.ap.max_connection = 1;
  ap_config.ap.authmode = WIFI_AUTH_OPEN;

  if (!setup_recovery && s_settings.setup_ap_pass[0] != '\0') {
    strlcpy((char *)ap_config.ap.password, s_settings.setup_ap_pass, sizeof(ap_config.ap.password));
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  }

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
  esp_netif_ip_info_t ip_info;
  ESP_ERROR_CHECK(esp_netif_get_ip_info(s_ap_netif, &ip_info));
  s_ap_ip = ip_info.ip;

  // Configure DHCP before esp_wifi_start triggers AP_START/DHCP startup.
  esp_netif_dns_info_t dns = {0};
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  dns.ip.u_addr.ip4 = s_ap_ip;
  ESP_ERROR_CHECK(esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns));
  uint8_t offer_dns = OFFER_DNS;
  ESP_ERROR_CHECK(esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
      ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns)));
  // Option 114 identifies an RFC 8908 API, not this HTML page.
  // Use captive DNS and HTTP probe redirects for the setup portal.
  if (!sta_running) ESP_ERROR_CHECK(esp_wifi_start());
  wifi_driver_started = true;

  ESP_LOGI(TAG, "Setup AP started: %s", ap_ssid);
  ESP_LOGI(TAG, "AP IP: " IPSTR, IP2STR(&s_ap_ip));

}

static int dns_skip_name(const uint8_t *buf, int len, int off)
{
  while (off < len) {
    uint8_t c = buf[off];
    if (c == 0) return off + 1;
    if ((c & 0xC0) == 0xC0) return off + 2;
    off += c + 1;
  }
  return -1;
}

static int dns_build_answer(uint8_t *resp, int req_len, uint32_t ip_be)
{
  if (req_len < 12 || req_len > DNS_MAX_LEN || (resp[2] & 0xf8) != 0 ||
      resp[4] != 0 || resp[5] != 1) return -1;
  int qname_end = dns_skip_name(resp, req_len, 12);
  if (qname_end < 0 || qname_end + 4 > req_len) return -1;
  uint16_t qtype = ((uint16_t)resp[qname_end] << 8) | resp[qname_end + 1];
  uint16_t qclass = ((uint16_t)resp[qname_end + 2] << 8) | resp[qname_end + 3];
  bool answer_a = qtype == 1 && qclass == 1;
  int pos = qname_end + 4;
  if (answer_a && pos + 16 > DNS_MAX_LEN) return -1;

  resp[2] = 0x80 | (resp[2] & 1); // Response, preserve recursion-desired.
  resp[3] = 0x80;
  resp[6] = 0; resp[7] = answer_a ? 1 : 0;
  resp[8] = resp[9] = resp[10] = resp[11] = 0;
  if (!answer_a) return pos; // No IPv6 address is offered by this portal.
  const uint8_t record[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
  memcpy(resp + pos, record, sizeof(record));
  pos += sizeof(record);
  memcpy(resp + pos, &ip_be, 4);
  return pos + 4;
}

static void dns_server_task(void *arg)
{
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) {
    ESP_LOGE(TAG, "DNS socket create failed");
    xEventGroupSetBits(s_wifi_event_group, DNS_DONE_BIT);
    vTaskDelete(NULL);
    return;
  }

  struct timeval timeout = { .tv_sec = 0, .tv_usec = 500000 };
  if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
    close(sock);
    xEventGroupSetBits(s_wifi_event_group, DNS_DONE_BIT);
    vTaskDelete(NULL);
    return;
  }
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(DNS_PORT);
  addr.sin_addr.s_addr = s_ap_ip.addr;

  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "DNS bind failed");
    close(sock);
    xEventGroupSetBits(s_wifi_event_group, DNS_DONE_BIT);
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "DNS server started");

  while (xEventGroupGetBits(s_wifi_event_group) & DNS_RUN_BIT) {
    uint8_t req[DNS_MAX_LEN];
    struct sockaddr_in from = {0};
    socklen_t from_len = sizeof(from);

    int len = recvfrom(sock, req, sizeof(req), 0,
                        (struct sockaddr *)&from, &from_len);
    if (len < 12) continue;

    uint16_t qdcount = ((uint16_t)req[4] << 8) | req[5];
    if (qdcount == 0) continue;

    uint8_t resp[DNS_MAX_LEN];
    memcpy(resp, req, len);

    int resp_len = dns_build_answer(resp, len, s_ap_ip.addr);
    if (resp_len > 0) {
      sendto(sock, resp, resp_len, 0,
            (struct sockaddr *)&from, from_len);
    }
  }
  close(sock);
  xEventGroupSetBits(s_wifi_event_group, DNS_DONE_BIT);
  vTaskDelete(NULL);
}

static void start_dns_server(void)
{
  xEventGroupClearBits(s_wifi_event_group, DNS_DONE_BIT);
  xEventGroupSetBits(s_wifi_event_group, DNS_RUN_BIT);
  if (xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 4, NULL) != pdPASS) {
    xEventGroupClearBits(s_wifi_event_group, DNS_RUN_BIT);
    xEventGroupSetBits(s_wifi_event_group, DNS_DONE_BIT);
    ESP_LOGE(TAG, "DNS task creation failed");
  }
}

static void stop_portal_servers(void)
{
  xEventGroupClearBits(s_wifi_event_group, DNS_RUN_BIT);
  if (s_httpd != NULL) {
    esp_err_t err;
    while ((err = httpd_stop(s_httpd)) != ESP_OK) {
      ESP_LOGE(TAG, "HTTP stop failed (%d); retrying", (int)err);
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_httpd = NULL;
  }
  xEventGroupWaitBits(s_wifi_event_group, DNS_DONE_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));
}

static void portal_cleanup_task(void *arg)
{
  // HTTP socket shutdown may wait for a client; keep this off the JJY task.
  stop_portal_servers();
  vTaskDelete(NULL);
}

static esp_err_t httpd_resp_send_printf(httpd_req_t *req, const char *fmt, ...)
{
    char line[SEND_BUF_SIZE];

    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    return httpd_resp_sendstr_chunk(req, line);
}

static void html_tz_option(httpd_req_t *req, float val, const char *label)
{
  httpd_resp_send_printf(req,
    "<option value='%.2f'%s>%s</option>",
    val,
    (fabsf(val - s_settings.timezone) < 0.01f) ? " selected" : "",
    label);
}

static void html_send_escaped(httpd_req_t *req, const char *text)
{
  char chunk[256];
  size_t used = 0;
  for (; *text; text++) {
    const char *entity = NULL;
    switch (*text) {
    case '&': entity = "&amp;"; break;
    case '<': entity = "&lt;"; break;
    case '>': entity = "&gt;"; break;
    case '\'': entity = "&#39;"; break;
    case '"': entity = "&quot;"; break;
    }
    size_t len = entity ? strlen(entity) : 1;
    if (used + len >= sizeof(chunk)) {
      chunk[used] = '\0';
      httpd_resp_sendstr_chunk(req, chunk);
      used = 0;
    }
    if (entity) memcpy(chunk + used, entity, len);
    else chunk[used] = *text;
    used += len;
  }
  if (used) { // A zero-length HTTP chunk would terminate the response.
    chunk[used] = '\0';
    httpd_resp_sendstr_chunk(req, chunk);
  }
}

static void httpd_resp_send_header(httpd_req_t *req)
{
  httpd_resp_send_printf(req,
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>" WIFI_AP_TITLE "</title>", (int)jjysim_board_get());
  httpd_resp_sendstr_chunk(req,
    "<style>" WIFI_AP_CSS_STYLE "</style></head><body>");
}

static void httpd_resp_send_footer(httpd_req_t *req)
{
  httpd_resp_sendstr_chunk(req, "</body></html>");
  httpd_resp_sendstr_chunk(req, NULL);
}

static inline const char *get_checed_str(bool val)
{
  return val ? "checked" : "";
}

static void html_ipv4_input(httpd_req_t *req, const char *name, const char *label,
                            const char *value, const char *placeholder)
{
  httpd_resp_send_printf(req,
    "<label for='%s'>%s</label>"
    "<input type='text' id='%s' name='%s' value='",
    name, label, name, name);
  html_send_escaped(req, value);
  httpd_resp_send_printf(req,
    "' placeholder='%s' "
    "inputmode='decimal' maxlength='15' pattern='[0-9.]{7,15}' required %s>",
    placeholder, s_settings.wifi_static_ip ? "" : "disabled");
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
  html_begin(req);
  httpd_resp_send_header(req);

  httpd_resp_sendstr_chunk(req, 
    "<button id='dummy' style='position:absolute; top:-1000px;'></button>"
    "<script>window.addEventListener('load',()=>{"
    "document.getElementById('dummy').focus();});"
    "</script>");

  httpd_resp_send_printf(req, "<h1>" WIFI_AP_TITLE "</h1>", (int)jjysim_board_get());
  httpd_resp_sendstr_chunk(req, "<form method='POST' action='/save'>");

  httpd_resp_sendstr_chunk(req, "<fieldset><legend>Wi-Fi</legend>");
  httpd_resp_sendstr_chunk(req, "<label for='ssid-select'>SSID</label><select id='ssid-select' name='" POST_NAME_SSID "'>");

  bool saved_ssid_found = false;
  for (int i = 0; i < s_scan_count; i++) {
    const char *ssid = (const char *)s_scan_records[i].ssid;
    if (ssid[0] == '\0') continue;
    if (strcmp(ssid, s_settings.ssid) == 0) saved_ssid_found = true;

      httpd_resp_sendstr_chunk(req, "<option value='");
      html_send_escaped(req, ssid);
      httpd_resp_send_printf(req, "'%s>", (strcmp(ssid, s_settings.ssid) == 0) ? " selected" : "");
      html_send_escaped(req, ssid);
      httpd_resp_send_printf(req, " (RSSI=%d)</option>", s_scan_records[i].rssi);
  }
  if (!saved_ssid_found && s_settings.ssid[0] != '\0') {
    httpd_resp_sendstr_chunk(req, "<option value='");
    html_send_escaped(req, s_settings.ssid);
    httpd_resp_sendstr_chunk(req, "' selected>");
    html_send_escaped(req, s_settings.ssid);
    httpd_resp_sendstr_chunk(req, "</option>");
  }
  httpd_resp_sendstr_chunk(req,
    "<option value=''>手入力する…</option></select>"
    "<div id='ssid-manual-area' hidden>"
      "<label for='ssid-manual'>SSIDを入力</label>"
      "<input type='text' id='ssid-manual' name='" POST_NAME_SSID_MANUAL "' "
        "maxlength='32' autocomplete='off' autocapitalize='none' spellcheck='false' disabled>"
    "</div>");

  httpd_resp_sendstr_chunk(req,
    "<label>Password</label>"
    "<input type='password' name='" POST_NAME_PASS "' value='");
  html_send_escaped(req, s_settings.pass);
  httpd_resp_sendstr_chunk(req, "' style='width:100%'>");

  httpd_resp_send_printf(req,
    "<label for='" POST_NAME_IP_MODE "'>IP address mode</label>"
    "<select id='" POST_NAME_IP_MODE "' name='" POST_NAME_IP_MODE "'>"
    "<option value='0'%s>DHCP (Auto)</option>"
    "<option value='1'%s>Static IP</option></select>"
    "<div id='static_ip_area'%s>",
    s_settings.wifi_static_ip ? "" : " selected",
    s_settings.wifi_static_ip ? " selected" : "",
    s_settings.wifi_static_ip ? "" : " hidden");
  html_ipv4_input(req, POST_NAME_IP_ADDR, "IP address", s_settings.wifi_ip, "192.168.1.100");
  html_ipv4_input(req, POST_NAME_IP_MASK, "Subnet mask", s_settings.wifi_netmask, "255.255.255.0");
  html_ipv4_input(req, POST_NAME_IP_GW, "Gateway", s_settings.wifi_gateway, "192.168.1.1");
  html_ipv4_input(req, POST_NAME_IP_DNS, "DNS server", s_settings.wifi_dns, "192.168.1.1");
  httpd_resp_sendstr_chunk(req,
    "<p class='footer-note'>Enter your network settings. "
    "Use an unused IP address.</p></div>");

  httpd_resp_sendstr_chunk(req, "</fieldset>");

  httpd_resp_sendstr_chunk(req,
    "<fieldset><legend>Setup Wi-Fi</legend>"
    "<label>AP password (blank = Open)</label>"
    "<input type='password' name='" POST_NAME_AP_PASS "' minlength='8' maxlength='63' value='");
  html_send_escaped(req, s_settings.setup_ap_pass);
  httpd_resp_sendstr_chunk(req, "' autocomplete='new-password'>");
  httpd_resp_send_printf(req,
    "<label>AP timeout (min, 0 = Off)</label>"
    "<input type='number' name='" POST_NAME_AP_WINDOW "' min='0' max='%d' value='%d' required style='width:120px'>"
    , SETUP_AP_WINDOW_MAX_SEC / 60, s_settings.setup_ap_window_sec / 60);
  httpd_resp_send_printf(req,
    "<label class='checkrow'>"
      "<input type='checkbox' name='setup_sta' value='1' %s>"
      "Allow setup access over Wi-Fi"
    "</label></fieldset>", get_checed_str(s_settings.setup_sta_enabled));

  httpd_resp_sendstr_chunk(req, "<fieldset><legend>JJY</legend>");

  httpd_resp_sendstr_chunk(req, "<label>Band</label><select name='" POST_NAME_BAND "'>");
  httpd_resp_sendstr_chunk(req, s_settings.band == 40 ?
    "<option value='40' selected>40kHz</option><option value='60'>60kHz</option>" :
    "<option value='40'>40kHz</option><option value='60' selected>60kHz</option>");
  httpd_resp_sendstr_chunk(req, "</select>");

  httpd_resp_sendstr_chunk(req, "<label>Output level</label><select name='" POST_NAME_OUTPUT "'>");
  httpd_resp_send_printf(req, "<option value='0'%s>Low</option>", s_settings.output_level == JJY_OUTPUT_LOW ? " selected" : "");
  httpd_resp_send_printf(req, "<option value='1'%s>Mid</option>", s_settings.output_level == JJY_OUTPUT_MID ? " selected" : "");
  httpd_resp_send_printf(req, "<option value='2'%s>High</option></select>", s_settings.output_level == JJY_OUTPUT_HIGH ? " selected" : "");

  httpd_resp_send_printf(req,
    "<label class='checkrow'>"
    "<input type='checkbox' name='" POST_NAME_PPS_TX_ONLY "' value='1' %s>"
    "PPS only while sending</label>",
    get_checed_str(s_settings.pps_tx_only));

  httpd_resp_sendstr_chunk(req, "<label>Timezone</label><select name='" POST_NAME_TZ "'>");

  html_tz_option(req, -12.0f, "(UTC-12:00) Baker Island");
  html_tz_option(req, -11.0f, "(UTC-11:00) American Samoa");
  html_tz_option(req, -10.0f, "(UTC-10:00) Hawaii");
  html_tz_option(req,  -9.0f, "(UTC-09:00) Alaska");
  html_tz_option(req,  -8.0f, "(UTC-08:00) Los Angeles / Pacific Time");
  html_tz_option(req,  -7.0f, "(UTC-07:00) Denver / Mountain Time");
  html_tz_option(req,  -6.0f, "(UTC-06:00) Chicago / Central Time");
  html_tz_option(req,  -5.0f, "(UTC-05:00) New York / Eastern Time");
  html_tz_option(req,  -4.0f, "(UTC-04:00) Santiago / Atlantic Time");
  html_tz_option(req,  -3.5f, "(UTC-03:30) Newfoundland");
  html_tz_option(req,  -3.0f, "(UTC-03:00) Buenos Aires / Sao Paulo");
  html_tz_option(req,  -2.0f, "(UTC-02:00) South Georgia");
  html_tz_option(req,  -1.0f, "(UTC-01:00) Azores");
  html_tz_option(req,   0.0f, "(UTC+00:00) London / Lisbon");
  html_tz_option(req,   1.0f, "(UTC+01:00) Berlin / Paris");
  html_tz_option(req,   2.0f, "(UTC+02:00) Cairo / Athens");
  html_tz_option(req,   3.0f, "(UTC+03:00) Moscow / Nairobi");
  html_tz_option(req,   3.5f, "(UTC+03:30) Tehran");
  html_tz_option(req,   4.0f, "(UTC+04:00) Dubai / Baku");
  html_tz_option(req,   4.5f, "(UTC+04:30) Kabul");
  html_tz_option(req,   5.0f, "(UTC+05:00) Karachi / Tashkent");
  html_tz_option(req,   5.5f, "(UTC+05:30) India / Colombo");
  html_tz_option(req,   5.75f,"(UTC+05:45) Nepal");
  html_tz_option(req,   6.0f, "(UTC+06:00) Dhaka / Almaty");
  html_tz_option(req,   6.5f, "(UTC+06:30) Yangon / Cocos Islands");
  html_tz_option(req,   7.0f, "(UTC+07:00) Bangkok / Jakarta");
  html_tz_option(req,   8.0f, "(UTC+08:00) Beijing / Singapore");
  html_tz_option(req,   9.0f, "(UTC+09:00) Tokyo / Seoul");
  html_tz_option(req,   9.5f, "(UTC+09:30) Darwin / Adelaide");
  html_tz_option(req,  10.0f, "(UTC+10:00) Sydney / Guam");
  html_tz_option(req,  11.0f, "(UTC+11:00) Solomon Islands / Magadan");
  html_tz_option(req,  12.0f, "(UTC+12:00) Fiji / Auckland");
  html_tz_option(req,  13.0f, "(UTC+13:00) Tonga / Samoa");

  httpd_resp_sendstr_chunk(req, "</select>");

  httpd_resp_send_printf(req,
    "<label class='checkrow'>"
    "<input type='checkbox' name='" POST_NAME_DST "' value='1' %s>"
    "DST (+1 hour)"
    "</label>",
    get_checed_str(s_settings.dst)
  );

  httpd_resp_send_printf(req,
    "<label class='checkrow'>"
    "<input type='checkbox' id='" POST_NAME_HOURLY "' name='" POST_NAME_HOURLY "' value='1' %s>"
    "Hourly mode"
    "</label>",
    get_checed_str(s_settings.hourly_mode)
  );

  httpd_resp_send_printf(req,
    "<div id='hourly_area' class='setting'>"
      "Active range: ±"
      "<input type='number' name='" POST_NAME_RANGE "' min='1' max='29' value='%d'>"
      "min (1-29)"
    "</div>",
    s_settings.hourly_range
  );

  httpd_resp_send_printf(req,
    "<div id='ofs_tm' class='setting'>"
      "Offset time: ±"
      "<input type='number' name='" POST_NAME_OFFSET"' min='-30' max='30' value='%d'>"
      "min (-30 to 30)"
    "</div>",
    s_settings.offset_time
  );

  httpd_resp_sendstr_chunk(req, "</fieldset>");

  httpd_resp_sendstr_chunk(req, "<fieldset><legend>Display</legend>");

  httpd_resp_sendstr_chunk(req, "<label>Initial display mode</label><select name='" POST_NAME_DISP "'>");
  httpd_resp_sendstr_chunk(req, s_settings.disp_mode ?
    "<option value='0'>Clock</option><option value='1' selected>Status</option>" :
    "<option value='0' selected>Clock</option><option value='1'>Status</option>");
  httpd_resp_sendstr_chunk(req, "</select>");

  httpd_resp_send_printf(req,
    "<label>Screen saver trigger</label><select id='" POST_NAME_SCREEN_TRIGGER "' name='" POST_NAME_SCREEN_TRIGGER "'>"
    "<option value='0'%s>After timeout</option>"
    "<option value='1'%s>While JJY is off</option></select>",
    s_settings.screensaver_trigger == SCREENSAVER_TRIGGER_TIMEOUT ? " selected" : "",
    s_settings.screensaver_trigger == SCREENSAVER_TRIGGER_JJY_OFF ? " selected" : "");
  httpd_resp_send_printf(req,
    "<label>Screen saver delay (min, 0 = Always on)</label>"
    "<input type='number' id='" POST_NAME_SCREENSAVER "' name='" POST_NAME_SCREENSAVER "' min='0' max='%d' value='%d' %s style='width:120px'>",
    DISPLAY_TIMEOUT_MAX_MIN, s_settings.screensaver_sec / 60,
    s_settings.screensaver_trigger == SCREENSAVER_TRIGGER_JJY_OFF ? "disabled" : "required");
  httpd_resp_send_printf(req,
    "<label>Screen saver type</label><select name='" POST_NAME_SCREEN_OFF "'>"
    "<option value='0'%s>Clock (Random)</option>"
    "<option value='1'%s>Screen off</option></select>",
    s_settings.screensaver_off ? "" : " selected",
    s_settings.screensaver_off ? " selected" : "");

  httpd_resp_sendstr_chunk(req,
    "<label for='bright'>OLED brightness</label>"
    "<div style='display:flex;align-items:center;gap:6px;'>"
      "<span>🌙</span>"
  );

  httpd_resp_send_printf(req,
      "<input type='range' id='bright' name='" POST_NAME_BRIGHT "' min='0' max='10' value='"
      "%d' oninput='setBrightness(this.value)'>"
      "<span>☀️</span>"
    "</div>",
    s_settings.brightness
  );

  httpd_resp_send_printf(req,
    "<label class='checkrow'>"
      "<input type='checkbox' name='" POST_NAME_NIGHT "' value='1' %s>"
      "Night mode (22:00–7:00)"
    "</label>",
    get_checed_str(s_settings.night_mode)
  );

  httpd_resp_sendstr_chunk(req, "</fieldset>");

  httpd_resp_sendstr_chunk(req, "<fieldset><legend>Time sync</legend>");

  httpd_resp_send_printf(req,
    "<label class='radiorow'>"
      "<input type='radio' name='" POST_NAME_NTP_MODE "' value='0' %s>"
      "<span class='ntp-label'>Default server ( "NTP_URL" )</span>"
    "</label>",
    get_checed_str(!s_settings.ntp_mode)
  );

  httpd_resp_send_printf(req,   
    "<label class='radiorow'>"
      "<input type='radio' name='" POST_NAME_NTP_MODE "' value='1' %s>"
      "<span class='ntp-label'>Custom server</span>"
    "</label>",
    get_checed_str(s_settings.ntp_mode)
  );

  httpd_resp_sendstr_chunk(req,
    "<div class='ntp-input'>"
      "<input type='text' id='" POST_NAME_NTP_URL "' name='" POST_NAME_NTP_URL "' "
      "value='");
  html_send_escaped(req, s_settings.ntp_url);
  httpd_resp_send_printf(req,
      "' %s placeholder='pool.ntp.org'>"
    "</div>",
    s_settings.ntp_mode ? "" : "disabled"
  );

  httpd_resp_sendstr_chunk(req, "</fieldset>");

  httpd_resp_sendstr_chunk(req, "<fieldset><legend>NTP server</legend>");
  httpd_resp_send_printf(req,
    "<label class='checkrow'>"
      "<input type='checkbox' name='" POST_NAME_NTP_SERVER "' value='1' %s>"
      "Enable"
    "</label>", get_checed_str(s_settings.ntp_server_enabled));
  httpd_resp_sendstr_chunk(req, "</fieldset>");

  httpd_resp_sendstr_chunk(req,
    "<button type='submit'>設定保存＆再起動</button></form>"
    "<div class='actions'>"
    "<p><a href='/update'><button type='button'>アップデート</button></a></p>"
    "<hr><form id='scan-form' method='POST' action='/scan' onsubmit='return beginWifiScan(this)'>"
    "<button type='submit'>Wi-Fiを再スキャン</button></form>"
    "<form method='POST' action='/clear_settings'>"
    "<button type='submit'>設定クリア＆再起動</button></form>"
    "<form method='POST' action='/reboot'>"
    "<button type='submit'>再起動</button></form>"
    "</div>"
    "<div class='nav'>"
    "<a href='/info'>デバイス情報</a>"
    "</div>"
    "<div id='scan-overlay' class='scan-overlay' role='dialog' aria-modal='true' aria-labelledby='scan-title' hidden>"
      "<div class='scan-dialog' role='status' aria-live='polite'>"
        "<div class='scan-spinner' aria-hidden='true'></div>"
        "<h2 id='scan-title'>Wi-Fiを検索しています…</h2>"
        "<p>しばらくお待ちください。</p>"
      "</div>"
    "</div>"
    "<script>"
      // Paint the in-page dialog before submitting; captive browsers may suppress native dialogs.
      "let scanSubmitTimer;"
      "function beginWifiScan(form){"
        "if(form.dataset.scanning==='1')return false;"
        "form.dataset.scanning='1';"
        "document.getElementById('scan-overlay').hidden=false;"
        "form.querySelector('button').disabled=true;"
        "scanSubmitTimer=setTimeout(()=>HTMLFormElement.prototype.submit.call(form),100);"
        "return false;"
      "}"
      "window.addEventListener('pageshow',()=>{"
        "clearTimeout(scanSubmitTimer);"
        "const form=document.getElementById('scan-form');"
        "form.dataset.scanning='';"
        "form.querySelector('button').disabled=false;"
        "document.getElementById('scan-overlay').hidden=true;"
      "});"
    "</script>"
  );

  httpd_resp_sendstr_chunk(req,
    "<script>"
    // ===== BRIGHTNESS =====
    "let brightTimer;"
    "function setBrightness(v){"
      "clearTimeout(brightTimer);"
      "brightTimer = setTimeout(()=>{"
        "fetch('/set_brightness', {"
          "method:'POST',"
          "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
          "body:'" POST_NAME_BRIGHT "=' + encodeURIComponent(v)"
        "}).catch(()=>{});"
      "}, 150);"
    "}"
    "function setAreaEnabled(areaId, enabled){"
      "document.querySelectorAll('#'+areaId+' input, #'+areaId+' select, #'+areaId+' textarea')"
        ".forEach(el => el.disabled = !enabled);"
    "}"
    "document.addEventListener('DOMContentLoaded', () => {"
      // ===== SSID manual entry =====
      "const ssidSelect = document.getElementById('ssid-select');"
      "const ssidManual = document.getElementById('ssid-manual');"
      "const updateSsidMode = () => {"
        "const manual = ssidSelect.value === '';"
        "document.getElementById('ssid-manual-area').hidden = !manual;"
        "ssidManual.disabled = !manual;"
        "ssidManual.required = manual;"
      "};"
      "ssidSelect.addEventListener('change', updateSsidMode);"
      "updateSsidMode();"
      // ===== IP mode =====
      "const ipMode = document.getElementById('" POST_NAME_IP_MODE "');"
      "const ipArea = document.getElementById('static_ip_area');"
      "const updateIpMode = () => {"
        "const enabled = ipMode.value === '1';"
        "ipArea.hidden = !enabled;"
        "setAreaEnabled('static_ip_area', enabled);"
      "};"
      "ipMode.addEventListener('change', updateIpMode);"
      "updateIpMode();"
      // ===== Screen saver trigger =====
      "const saverTrigger = document.getElementById('" POST_NAME_SCREEN_TRIGGER "');"
      "const saverDelay = document.getElementById('" POST_NAME_SCREENSAVER "');"
      "const updateSaverTrigger = () => {"
        "const timed = saverTrigger.value === '0';"
        "saverDelay.disabled = !timed;"
        "saverDelay.required = timed;"
      "};"
      "saverTrigger.addEventListener('change', updateSaverTrigger);"
      "updateSaverTrigger();"
      // ===== NTP =====
      "const radios = document.querySelectorAll('input[name=\"" POST_NAME_NTP_MODE "\"]');"
      "const input = document.getElementById('" POST_NAME_NTP_URL "');"
      "const updateNtp = () => {"
        "input.disabled = radios[0].checked;"
      "};"
      "radios.forEach(r => r.addEventListener('change', updateNtp));"
      "updateNtp();"
      // ===== Hourly =====
      "const hourlyCheckbox = document.getElementById('" POST_NAME_HOURLY "');"
      "const hourlyArea = document.getElementById('hourly_area');"
      "if(hourlyCheckbox && hourlyArea){"
        "const updateHourly = () => {"
          "const enabled = hourlyCheckbox.checked;"
          "setAreaEnabled('hourly_area', enabled);"
          "hourlyArea.classList.toggle('disabled', !enabled);"
        "};"
        "hourlyCheckbox.addEventListener('change', updateHourly);"
        "updateHourly();"
      "}"
    "});"
    "</script>"
  );
  httpd_resp_send_footer(req);

  return ESP_OK;
}

static esp_err_t httpd_resp_send_text(httpd_req_t *req, char *text)
{
  html_begin(req);
  httpd_resp_send_header(req);
  httpd_resp_sendstr_chunk(req, text);
  httpd_resp_send_footer(req);
  return ESP_OK;
}

static bool form_get_minutes(const char *body, const char *key, int *minutes)
{
  char value[32];
  if (!form_get_value(body, key, value, sizeof(value)) || value[0] == '\0' ||
      strspn(value, "0123456789") != strlen(value)) return false;
  char *end;
  long parsed = strtol(value, &end, 10);
  if (*end != '\0' || parsed < 0 || parsed > DISPLAY_TIMEOUT_MAX_MIN) return false;
  *minutes = (int)parsed;
  return true;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
  if (req->content_len == 0 || req->content_len > 2048) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body size");
    return ESP_FAIL;
  }
  char *buf = malloc(req->content_len + 1);
  if (!buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_FAIL;
  }
  size_t received = 0;
  while (received < req->content_len) {
    int len = httpd_req_recv(req, buf + received, req->content_len - received);
    if (len <= 0) {
      free(buf);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete body");
      return ESP_FAIL;
    }
    received += len;
  }
  buf[received] = '\0';
  if (memchr(buf, '\0', received)) {
    free(buf);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
    return ESP_FAIL;
  }

  app_settings_t cfg = s_settings;
  char temp[64];

  if (!form_get_ssid(buf, cfg.ssid, sizeof(cfg.ssid)) ||
      !form_get_value(buf, POST_NAME_PASS"=", cfg.pass, sizeof(cfg.pass))) {
    free(buf);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SSID or password field");
    return ESP_FAIL;
  }
  if (!form_get_value(buf, POST_NAME_AP_PASS "=", cfg.setup_ap_pass, sizeof(cfg.setup_ap_pass)) ||
      !settings_ap_password_valid(cfg.setup_ap_pass)) {
    free(buf);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "AP password must be empty or 8-63 printable ASCII characters");
    return ESP_FAIL;
  }
  if (!form_get_value(buf, POST_NAME_SCREEN_TRIGGER "=", temp, sizeof(temp)) ||
      (strcmp(temp, "0") != 0 && strcmp(temp, "1") != 0)) {
    free(buf);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid screen saver trigger");
    return ESP_FAIL;
  }
  cfg.screensaver_trigger = (screensaver_trigger_t)(temp[0] - '0');
  int window_min, screensaver_min;
  if (!form_get_minutes(buf, POST_NAME_AP_WINDOW "=", &window_min) ||
      (cfg.screensaver_trigger == SCREENSAVER_TRIGGER_TIMEOUT &&
       !form_get_minutes(buf, POST_NAME_SCREENSAVER "=", &screensaver_min))) {
    free(buf);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid time in minutes");
    return ESP_FAIL;
  }
  cfg.setup_ap_window_sec = window_min * 60;
  // A disabled HTML input is not submitted; preserve the previous timeout.
  if (cfg.screensaver_trigger == SCREENSAVER_TRIGGER_TIMEOUT) cfg.screensaver_sec = screensaver_min * 60;
  if (!form_get_value(buf, POST_NAME_SCREEN_OFF "=", temp, sizeof(temp)) ||
      (strcmp(temp, "0") != 0 && strcmp(temp, "1") != 0)) {
    free(buf);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid display timeout action");
    return ESP_FAIL;
  }
  cfg.screensaver_off = (temp[0] == '1');
  form_get_value(buf, "setup_sta=", temp, sizeof(temp));
  cfg.setup_sta_enabled = (strcmp(temp, "1") == 0);
  cfg.wifi_valid = (cfg.ssid[0] != '\0');
  if (!form_get_value(buf, POST_NAME_OUTPUT "=", temp, sizeof(temp)) ||
      strlen(temp) != 1 || temp[0] < '0' || temp[0] > '2') {
    free(buf);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid output level");
    return ESP_FAIL;
  }
  cfg.output_level = temp[0] - '0';
  form_get_value(buf, POST_NAME_PPS_TX_ONLY "=", temp, sizeof(temp));
  cfg.pps_tx_only = (strcmp(temp, "1") == 0);

  form_get_value(buf, POST_NAME_BAND"=", temp, sizeof(temp));
  if (temp[0]) cfg.band = atoi(temp);

  form_get_value(buf, POST_NAME_TZ"=", temp, sizeof(temp));
  if (temp[0]) cfg.timezone = strtof(temp, NULL);

  form_get_value(buf, POST_NAME_DST"=", temp, sizeof(temp));
  cfg.dst = temp[0];

  form_get_value(buf, POST_NAME_HOURLY"=", temp, sizeof(temp));
  cfg.hourly_mode = temp[0];

  form_get_value(buf, POST_NAME_RANGE"=", temp, sizeof(temp));
  if (temp[0]) cfg.hourly_range = atoi(temp);

  form_get_value(buf, POST_NAME_OFFSET"=", temp, sizeof(temp));
  if (temp[0]) cfg.offset_time = atoi(temp);

  form_get_value(buf, POST_NAME_DISP"=", temp, sizeof(temp));
  if (temp[0]) cfg.disp_mode = atoi(temp);

  form_get_value(buf, POST_NAME_BRIGHT"=", temp, sizeof(temp));
  if (temp[0]) cfg.brightness = atoi(temp);

  form_get_value(buf, POST_NAME_NIGHT"=", temp, sizeof(temp));
  cfg.night_mode = temp[0];

  form_get_value(buf, POST_NAME_NTP_SERVER "=", temp, sizeof(temp));
  cfg.ntp_server_enabled = (strcmp(temp, "1") == 0);

  form_get_value(buf, POST_NAME_NTP_MODE"=", temp, sizeof(temp));
  if (temp[0]) cfg.ntp_mode = atoi(temp);

  form_get_value(buf, POST_NAME_NTP_URL"=", cfg.ntp_url, sizeof(cfg.ntp_url));

  const char *ip_error = NULL;
  if (!form_get_value(buf, POST_NAME_IP_MODE "=", temp, sizeof(temp)) ||
      (strcmp(temp, "0") != 0 && strcmp(temp, "1") != 0)) {
    ip_error = "Select DHCP or Static IP.";
  } else {
    cfg.wifi_static_ip = (strcmp(temp, "1") == 0);
    if (cfg.wifi_static_ip) {
      if (!form_get_value(buf, POST_NAME_IP_ADDR "=", cfg.wifi_ip, sizeof(cfg.wifi_ip)) ||
          !form_get_value(buf, POST_NAME_IP_MASK "=", cfg.wifi_netmask, sizeof(cfg.wifi_netmask)) ||
          !form_get_value(buf, POST_NAME_IP_GW "=", cfg.wifi_gateway, sizeof(cfg.wifi_gateway)) ||
          !form_get_value(buf, POST_NAME_IP_DNS "=", cfg.wifi_dns, sizeof(cfg.wifi_dns))) {
        ip_error = "Enter all four IPv4 fields (maximum 15 characters each).";
      } else {
        esp_netif_ip_info_t info;
        esp_netif_dns_info_t dns;
        ip_error = static_ip_info(&cfg, &info, &dns);
      }
    }
  }
  free(buf);
  if (ip_error) {
    httpd_resp_set_status(req, "400 Bad Request");
    html_begin(req);
    httpd_resp_send_header(req);
    httpd_resp_send_printf(req,
      "<h2>Check IP settings</h2><p>%s</p>"
      "<p>Go back to edit your settings.</p>", ip_error);
    httpd_resp_send_footer(req);
    return ESP_OK;
  }

  esp_err_t err = save_settings(&cfg);
  if (err != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
    return ESP_FAIL;
  }

  s_settings = cfg;

  httpd_resp_send_text(req,
      "<h2>Settings saved</h2>"
      "<h3>Rebooting...</h3>");
  reboot_req_handler();
  return ESP_OK;
}

static esp_err_t scan_post_handler(httpd_req_t *req)
{
  esp_err_t err = start_wifi_scan();
  if (err != ESP_OK) {
    html_begin(req);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_send_header(req);
    httpd_resp_sendstr_chunk(req,
      "<h2>Wi-Fiの検索に失敗しました</h2>"
      "<p>設定画面に戻って、もう一度お試しください。</p>"
      "<div class='nav'><a href='/'>設定画面に戻る</a></div>");
    httpd_resp_send_footer(req);
    return ESP_OK;
  }
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", portal_root_location(req));
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t clear_settings_post_handler(httpd_req_t *req)
{
  esp_err_t err = clear_all_settings();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Settings clear failed: %s", esp_err_to_name(err));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to clear settings");
    return err;
  }
  httpd_resp_send_text(req,
      "<h2>Settings cleared</h2>"
      "<h3>Rebooting...</h3>");
  reboot_req_handler();
  return ESP_OK;
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
  httpd_resp_send_text(req,
      "<h2>Rebooting...</h2>");
  reboot_req_handler();
  return ESP_OK;
}

static esp_err_t captive_get_handler(httpd_req_t *req)
{
  redirect_root(req);
  return ESP_OK;
}

static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
  redirect_root(req);
  return ESP_OK;
}

static void format_mac_addr(char *out, size_t out_len, const uint8_t mac[6])
{
  snprintf(out, out_len,
           "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void html_send_kv_ex(httpd_req_t *req, const char *key, const char *value, bool mono)
{
  httpd_resp_send_printf(req,
           "<tr><th>%s</th><td%s>%s</td></tr>",
           key,
           mono ? " class='mono'" : "",
           value ? value : "");
}

static void html_send_kv(httpd_req_t *req, const char *key, const char *value)
{
  html_send_kv_ex(req, key, value, false);
}

static void html_send_kv_mono(httpd_req_t *req, const char *key, const char *value)
{
  html_send_kv_ex(req, key, value, true);
}

static void html_send_fieldset_open(httpd_req_t *req, const char *title)
{
  httpd_resp_send_printf(req,
      "<fieldset><legend>%s</legend><table>",
      title);
}

static void html_send_fieldset_close(httpd_req_t *req)
{
  httpd_resp_sendstr_chunk(req, "</table></fieldset>");
}

static esp_err_t info_get_handler(httpd_req_t *req)
{
  html_begin(req);
  httpd_resp_send_header(req);

  httpd_resp_send_printf(req, "<h1>JJY-SIM R%d Device info</h1>", (int)jjysim_board_get());

  html_send_fieldset_open(req, "Product");
  html_send_kv(req, "Name", JJYSIM_PRODUCT_NAME);
  char board_rev[8];
  snprintf(board_rev, sizeof(board_rev), "R%d", (int)jjysim_board_get());
  html_send_kv(req, "Board revision", board_rev);
  html_send_kv(req, "Vendor", JJYSIM_VENDOR_NAME);
  html_send_fieldset_close(req);

  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);

  const esp_app_desc_t *app = esp_app_get_description();

  uint8_t sta_mac[6] = {0};
  uint8_t ap_mac[6]  = {0};
  char sta_mac_str[32];
  char ap_mac_str[32];
  char temp[128];

  esp_read_mac(sta_mac, ESP_MAC_WIFI_STA);
  esp_read_mac(ap_mac, ESP_MAC_WIFI_SOFTAP);

  format_mac_addr(sta_mac_str, sizeof(sta_mac_str), sta_mac);
  format_mac_addr(ap_mac_str, sizeof(ap_mac_str), ap_mac);

  html_send_fieldset_open(req, "Device");
  html_send_kv(req, "Module", JJYSIM_MODULE_NAME);
  html_send_kv(req, "Chip", JJYSIM_CHIP_NAME);

  snprintf(temp, sizeof(temp), "%d", chip_info.revision);
  html_send_kv(req, "Chip revision", temp);

  uint32_t flash_size = 0;
  if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
    snprintf(temp, sizeof(temp), "%u MB",
             (unsigned int)(flash_size / (1024 * 1024)));
  } else {
    snprintf(temp, sizeof(temp), "unknown");
  }
  html_send_kv(req, "Flash size", temp);

  html_send_kv_mono(req, "STA MAC", sta_mac_str);
  html_send_kv_mono(req, "AP MAC", ap_mac_str);
  html_send_fieldset_close(req);

  snprintf(temp, sizeof(temp), "%s %s", app->date, app->time);

  html_send_fieldset_open(req, "Software");
  html_send_kv_mono(req, "Project", app->project_name);
  html_send_kv_mono(req, "Version", app->version);
  html_send_kv_mono(req, "ESP-IDF", app->idf_ver);
  html_send_kv_mono(req, "Build"  , temp);
  html_send_kv_mono(req, "Target" , CONFIG_IDF_TARGET);
  html_send_fieldset_close(req);

	snprintf(temp, sizeof(temp),"%s%s%s\n", 
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT " : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE " : "");

  html_send_fieldset_open(req, "Radio certification");
  html_send_kv(req, "Radio", temp);
  html_send_kv(req, "Module", JJYSIM_MODULE_NAME);
  html_send_kv_mono(req, "Certification ID", JJYSIM_CERTIFICATION);
  html_send_fieldset_close(req);

  html_send_fieldset_open(req, "License / Copyright");
  html_send_kv(req, "Copyright", JJYSIM_COPYRIGHT " " JJYSIM_VENDOR_NAME);
  html_send_kv(req, "License", JJYSIM_LICENSE_NAME);
  httpd_resp_sendstr_chunk(req,
    "<tr><th></th><td><a href='/license'>View License</a></td></tr>");
  html_send_fieldset_close(req);

  httpd_resp_sendstr_chunk(req,
    "<div class='nav'>"
    "<a href='/'>Back to Setup</a>"
    "</div>");

  httpd_resp_send_footer(req);
  return ESP_OK;
}

static esp_err_t license_get_handler(httpd_req_t *req)
{
  html_begin(req);
  httpd_resp_send_header(req);

  httpd_resp_sendstr_chunk(req, "<h1>MIT License</h1>");

  httpd_resp_sendstr_chunk(req,
    "<pre style='white-space:pre-wrap;font-family:Consolas,monospace;'>"
    WIFI_AP_MIT_LICENSE
    "</pre>");

  httpd_resp_sendstr_chunk(req,
    "<div class='nav'><a href='/info'>Back</a></div>");

  httpd_resp_send_footer(req);
  return ESP_OK;
}

static esp_err_t set_brightness_post_handler(httpd_req_t *req)
{
  char buf[128];
  int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (len <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    return ESP_FAIL;
  }
  buf[len] = '\0';

  char temp[32];
  form_get_value(buf, "bright=", temp, sizeof(temp));
  if (temp[0] == '\0') {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No bright");
    return ESP_FAIL;
  }

  s_settings.brightness = atoi(temp);
  disp_brightness(-1);

  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

// Match the accepted socket's local address, never a client-supplied Host header.
static bool portal_is_interface_request(httpd_req_t *req, esp_ip4_addr_t address)
{
  struct sockaddr_storage local = {0};
  socklen_t len = sizeof(local);
  if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &len) != 0) return false;

  if (local.ss_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
    const struct sockaddr_in *ip4 = (const struct sockaddr_in *)&local;
    return ip4->sin_addr.s_addr == address.addr;
  }
#if CONFIG_LWIP_IPV6
  if (local.ss_family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
    // IDF's dual-stack HTTP socket returns IPv4 peers/local addresses as ::ffff:a.b.c.d.
    const struct sockaddr_in6 *ip6 = (const struct sockaddr_in6 *)&local;
    const uint8_t mapped_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    const uint8_t *addr = ip6->sin6_addr.s6_addr;
    return memcmp(addr, mapped_prefix, sizeof(mapped_prefix)) == 0 &&
           memcmp(addr + sizeof(mapped_prefix), &address.addr, sizeof(address.addr)) == 0;
  }
#endif
  return false;
}

static esp_err_t portal_dispatch(httpd_req_t *req)
{
  bool via_sta = !portal_is_interface_request(req, s_ap_ip);
  if (via_sta) {
    esp_netif_ip_info_t info = {0};
    if (!s_settings.setup_sta_enabled || s_sta_netif == NULL ||
        esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK || info.ip.addr == 0 ||
        !portal_is_interface_request(req, info.ip)) {
      httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
      return ESP_FAIL;
    }
  }
  ESP_LOGI(TAG, "Portal HTTP: method=%d uri=%s", req->method, req->uri);
  bool root = req->method == HTTP_GET && (strcmp(req->uri, "/") == 0 || strncmp(req->uri, "/?", 2) == 0);
  if (root) {
    if (!portal_request_setup(via_sta)) {
      httpd_resp_set_status(req, "503 Service Unavailable");
      return httpd_resp_sendstr(req, "Setup window closed. Power cycle to reopen.");
    }
    // The main task stops PWM before exposing editable settings.
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, PORTAL_READY_BIT,
                                         pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
    if (!(bits & PORTAL_READY_BIT)) {
      httpd_resp_set_status(req, "503 Service Unavailable");
      return httpd_resp_sendstr(req, "Entering setup. Please reload.");
    }
  } else if (portal_get_state() != PORTAL_ACTIVE) {
    redirect_root(req);
    return ESP_OK;
  }
  if (via_sta && !sta_setup_active) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
  }
  esp_err_t (*handler)(httpd_req_t *) = req->user_ctx;
  // Prevent scans, saved configuration changes and restarts during OTA networking/Flash writes.
  if (ota_update_busy() && req->method == HTTP_POST && strncmp(req->uri, "/update/", 8) != 0) {
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "Update operation in progress. Please wait.");
  }
  return handler(req);
}

static void start_web_server(void)
{
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 17;
  // Captive probes and abrupt browser/Wi-Fi exits can leave idle TCP sessions.
  // Keep spare lwIP sockets for DNS/control and always admit a fresh connection.
  config.max_open_sockets = PORTAL_HTTP_MAX_CLIENTS;
  config.lru_purge_enable = true;
  config.keep_alive_enable = true;
  config.keep_alive_idle = 5;
  config.keep_alive_interval = 3;
  config.keep_alive_count = 3;

  esp_err_t err = httpd_start(&s_httpd, &config);
  if (err == ESP_OK) {
    httpd_uri_t root = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = portal_dispatch,
      .user_ctx = root_get_handler
    };
    httpd_uri_t info = {
      .uri = "/info",
      .method = HTTP_GET,
      .handler = portal_dispatch,
      .user_ctx = info_get_handler
    };
    httpd_uri_t save = {
      .uri = "/save",
      .method = HTTP_POST,
      .handler = portal_dispatch,
      .user_ctx = save_post_handler
    };
    httpd_uri_t scan = {
      .uri = "/scan",
      .method = HTTP_POST,
      .handler = portal_dispatch,
      .user_ctx = scan_post_handler
    };
    httpd_uri_t clear_settings = {
      .uri = "/clear_settings",
      .method = HTTP_POST,
      .handler = portal_dispatch,
      .user_ctx = clear_settings_post_handler
    };
    httpd_uri_t reboot = {
      .uri = "/reboot",
      .method = HTTP_POST,
      .handler = portal_dispatch,
      .user_ctx = reboot_post_handler
    };
    httpd_uri_t generate_204 = {
      .uri = "/generate_204",
      .method = HTTP_GET,
      .handler = portal_dispatch,
      .user_ctx = captive_get_handler
    };
    httpd_uri_t hotspot_detect = {
      .uri = "/hotspot-detect.html",
      .method = HTTP_GET,
      .handler = portal_dispatch,
      .user_ctx = captive_get_handler
    };
    httpd_uri_t ncsi = {
      .uri = "/ncsi.txt",
      .method = HTTP_GET,
      .handler = portal_dispatch,
      .user_ctx = captive_get_handler
    };
    httpd_uri_t connecttest = {
      .uri = "/connecttest.txt",
      .method = HTTP_GET,
      .handler = portal_dispatch,
      .user_ctx = captive_get_handler
    };
    httpd_uri_t fwlink = {
      .uri = "/fwlink",
      .method = HTTP_GET,
      .handler = portal_dispatch,
      .user_ctx = captive_get_handler
    };
    httpd_uri_t license = {
      .uri = "/license",
      .method = HTTP_GET,
      .handler = portal_dispatch,
      .user_ctx = license_get_handler
    };
    httpd_uri_t set_brightness_uri = {
      .uri      = "/set_brightness",
      .method   = HTTP_POST,
      .handler = portal_dispatch,
      .user_ctx = set_brightness_post_handler
    };

    httpd_uri_t update_routes[] = {
      {.uri="/update", .method=HTTP_GET, .handler=portal_dispatch, .user_ctx=ota_update_page},
      {.uri="/update/status", .method=HTTP_GET, .handler=portal_dispatch, .user_ctx=ota_update_status},
      {.uri="/update/check", .method=HTTP_POST, .handler=portal_dispatch, .user_ctx=ota_update_check},
      {.uri="/update/start", .method=HTTP_POST, .handler=portal_dispatch, .user_ctx=ota_update_start},
    };
    for (size_t i = 0; i < sizeof(update_routes)/sizeof(update_routes[0]); i++)
      ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &update_routes[i]));
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &info);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &clear_settings);
    httpd_register_uri_handler(s_httpd, &reboot);
    httpd_register_uri_handler(s_httpd, &generate_204);
    httpd_register_uri_handler(s_httpd, &hotspot_detect);
    httpd_register_uri_handler(s_httpd, &ncsi);
    httpd_register_uri_handler(s_httpd, &connecttest);
    httpd_register_uri_handler(s_httpd, &fwlink);
    httpd_register_uri_handler(s_httpd, &license);
    httpd_register_uri_handler(s_httpd, &set_brightness_uri);

    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, http_404_error_handler);

    ESP_LOGI(TAG, "HTTP server started");
  } else {
    ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
  }
}

// Called by the main task: only the attempt itself runs STA, not its retry gap.
static void start_sta_attempt(void)
{
  portal_state_t state = portal_get_state();
  if (setup_mode || state == PORTAL_REQUESTED || state == PORTAL_ACTIVE) return;
  esp_err_t err = esp_wifi_set_mode(state == PORTAL_WINDOW ? WIFI_MODE_APSTA : WIFI_MODE_STA);
  if (err == ESP_OK) {
    // IDF applies these defaults to the internal scan in esp_wifi_connect too.
    wifi_scan_default_params_t scan = WIFI_SCAN_PARAMS_DEFAULT_CONFIG();
    scan.home_chan_dwell_time = state == PORTAL_WINDOW ? AP_SCAN_HOME_DWELL_MS
                                                       : WIFI_SCAN_HOME_CHANNEL_DWELL_DEFAULT_TIME;
    err = esp_wifi_set_scan_parameters(&scan);
  }
  // A phone may have associated while STA was being enabled.
  state = portal_get_state();
  if (setup_mode || state == PORTAL_REQUESTED || state == PORTAL_ACTIVE) return;
  if (err == ESP_OK) err = esp_wifi_connect();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "STA attempt failed: %s", esp_err_to_name(err));
    portENTER_CRITICAL(&portal_lock);
    sta_retry_due_us = esp_timer_get_time() + STA_RETRY_DELAY_US;
    sta_pause_ap_pending = portal_state == PORTAL_WINDOW;
    portEXIT_CRITICAL(&portal_lock);
  }
}

void wifi_start_setup_window(void)
{
  setup_mode = false;
  sta_setup_active = false;
  xEventGroupClearBits(s_wifi_event_group, PORTAL_READY_BIT);
  if (s_settings.setup_ap_window_sec == 0) {
    // No temporary AP, HTTP or DNS listeners. Failures still enter persistent setup.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (!wifi_driver_started) ESP_ERROR_CHECK(esp_wifi_start());
    wifi_driver_started = true;
    start_sta_attempt();
    return;
  }
  wifi_start_apsta_for_setup(wifi_driver_started);
  portENTER_CRITICAL(&portal_lock);
  portal_deadline_us = esp_timer_get_time() + s_settings.setup_ap_window_sec * 1000000LL;
  portal_state = PORTAL_WINDOW;
  portEXIT_CRITICAL(&portal_lock);
  start_web_server();
  start_dns_server();
  ESP_LOGI(TAG, "Setup AP available for %d seconds from publication", s_settings.setup_ap_window_sec);
  // The AP, DHCP settings and portal listeners are ready before STA scans begin.
  start_sta_attempt();
}

bool wifi_setup_window_poll(void)
{
  bool expired = false;
  bool retry = false;
  bool pause_ap = false;
  int64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&portal_lock);
  if (portal_state == PORTAL_WINDOW && now >= portal_deadline_us) {
    portal_state = PORTAL_CLOSED;
    expired = true;
  }
  bool requested = portal_state == PORTAL_REQUESTED;
  if (sta_pause_ap_pending) {
    pause_ap = portal_state == PORTAL_WINDOW && !setup_mode;
    sta_pause_ap_pending = false;
  }
  if (!setup_mode && !requested && sta_retry_due_us && now >= sta_retry_due_us) {
    sta_retry_due_us = 0;
    retry = true;
  }
  portEXIT_CRITICAL(&portal_lock);
  if (expired) {
    xEventGroupClearBits(s_wifi_event_group, DNS_RUN_BIT);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (xTaskCreate(portal_cleanup_task, "portal_cleanup", 3072, NULL, 2, NULL) != pdPASS) {
      ESP_LOGE(TAG, "Portal cleanup task failed; stopping servers synchronously");
      // Do not leave the HTTP listener on STA if a cleanup task cannot be allocated.
      stop_portal_servers();
    }
    ESP_LOGI(TAG, "Setup AP window expired; continuing STA/JJY");
  }
  if (pause_ap && !retry) {
    // Stop STA between attempts so cached phone connections get a stable AP.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_LOGI(TAG, "STA retry gap: AP-only mode");
  }
  if (retry) start_sta_attempt();
  return requested;
}

void enter_setup_mode(void)
{
  ESP_LOGW(TAG, "Entering setup portal mode");
  portal_state_t state = portal_get_state();
  bool promote = state == PORTAL_REQUESTED || state == PORTAL_WINDOW;
  portENTER_CRITICAL(&portal_lock);
  setup_mode = true;
  sta_retry_due_us = 0;
  sta_pause_ap_pending = false;
  portEXIT_CRITICAL(&portal_lock);
  if (!sta_setup_active) {
    esp_wifi_disconnect();
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    wifi_status = WIFI_STATUS_UNKNOWN;
  }

  if (!promote) {
    esp_wifi_stop();
    wifi_driver_started = false;
    vTaskDelay(pdMS_TO_TICKS(300));
    wifi_start_apsta_for_setup(false);
    start_web_server();
    start_dns_server();
  }
  if (!sta_setup_active) {
    // AP-origin setup keeps the original AP-only behavior.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    start_wifi_scan();
  }
  // STA-origin setup borrows the live connection; do not disrupt the browser or OTA.
  portENTER_CRITICAL(&portal_lock);
  portal_state = PORTAL_ACTIVE;
  portEXIT_CRITICAL(&portal_lock);
  xEventGroupSetBits(s_wifi_event_group, PORTAL_READY_BIT);
}

void enter_setup_recovery_mode(void)
{
  setup_recovery = true;
  enter_setup_mode();
}

bool is_wifi_connected(void)
{
  EventBits_t bits = xEventGroupWaitBits(
      s_wifi_event_group,
      WIFI_CONNECTED_BIT,
      pdFALSE,
      pdFALSE,
      pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS)
  );
  return (bits & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_update_connect(void)
{
  if (!setup_mode || !s_settings.wifi_valid) return ESP_ERR_INVALID_STATE;
  if (sta_setup_active) {
    update_network = true;
    if (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) return ESP_OK;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) return err;
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                          pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    return bits & WIFI_CONNECTED_BIT ? ESP_OK : ESP_ERR_TIMEOUT;
  }
  wifi_config_t cfg = {0};
  memcpy(cfg.sta.ssid, s_settings.ssid, strnlen(s_settings.ssid, sizeof(cfg.sta.ssid)));
  strlcpy((char *)cfg.sta.password, s_settings.pass, sizeof(cfg.sta.password));
  cfg.sta.pmf_cfg.capable = true;
  xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | UPDATE_STA_READY_BIT);
  // Wait for the netif's STA_START processing before configuring IP or connecting.
  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (err != ESP_OK) return err;
  EventBits_t ready = xEventGroupWaitBits(s_wifi_event_group, UPDATE_STA_READY_BIT,
                                         pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
  if (!(ready & UPDATE_STA_READY_BIT)) return ESP_ERR_TIMEOUT;
  err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
  if (err != ESP_OK) return err;
  update_network = true;
  err = configure_sta_ip();
  if (err != ESP_OK) return err;
  err = esp_wifi_connect();
  if (err != ESP_OK) return err;
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                        pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
  return bits & WIFI_CONNECTED_BIT ? ESP_OK : ESP_ERR_TIMEOUT;
}

void wifi_update_disconnect(void)
{
  update_network = false;
  if (sta_setup_active) return; // A STA-origin browser still needs this connection.
  esp_wifi_disconnect();
  esp_wifi_set_mode(WIFI_MODE_AP);
  xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
}

char *get_wifi_addr_str(void)
{
  return ip_str;
}

const char *wifi_get_setup_addr_str(void)
{
  return sta_setup_active ? ip_str : "192.168.4.1";
}

bool is_reboot_req(void)
{
  return reboot_req;
}

wifi_status_t wifi_get_status(void)
{
  return wifi_status;
}
