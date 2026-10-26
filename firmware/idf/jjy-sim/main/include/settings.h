/*
 * JJY-SIM - Settings (API)
 *
 * Provides application settings management.
 *
 * - Defines configuration structure (Wi-Fi, band, timezone, etc.)
 * - Loads and saves settings using NVS
 * - Provides default initialization and partial reset
 *
 * Copyright (c) 2026 Shachi-lab
 * SPDX-License-Identifier: MIT
 */
#ifndef _SETTINGS_H_
#define _SETTINGS_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "jjy_pwm.h"

typedef enum {
  SCREENSAVER_TRIGGER_TIMEOUT = 0,
  SCREENSAVER_TRIGGER_JJY_OFF = 1
} screensaver_trigger_t;

typedef struct {
  char ssid[64];
  char pass[64];
  int band;         // JJY band (40 or 60 kHz)
  bool dst;         // Daylight saving (0:off, 1:on)
  bool hourly_mode; // True if sync assist is enabled
  int hourly_range; // Sync assist range
  float timezone;   // Timezone (e.g. 9.0, 5.5, 5.75)
  int disp_mode;    // Display mode (0:Normal, 1:Large)
  int brightness;   // Display brightness
  bool night_mode;  // True if night mode is enabled
  bool wifi_valid;  // True if valid Wi-Fi settings are loaded
  bool ntp_server_enabled; // Serve UTC to LAN clients; default off.
  bool ntp_mode;    // 1 if custom NTP server is used
  char ntp_url[128];// NTP server
  int offset_time;  // Offset time
  bool wifi_static_ip; // False: DHCP (also the default for existing devices)
  char wifi_ip[16];
  char wifi_netmask[16];
  char wifi_gateway[16];
  char wifi_dns[16];
  char setup_ap_pass[64];
  bool setup_sta_enabled; // Allow STA setup entry during the setup window; default off.
  int setup_ap_window_sec; // Stored in seconds for compatibility; whole-minute values.
  screensaver_trigger_t screensaver_trigger; // Default: elapsed time.
  bool screensaver_off; // False: moving clock; true: display off after timeout.
  int screensaver_sec; // Seconds; zero keeps the normal display always visible.
  bool pps_tx_only; // Suppress PPS pulses while JJY transmission is disabled.
  int output_level; // Low/Mid/High carrier duty; default High preserves existing output.
} app_settings_t;

// Global settings instance
extern app_settings_t s_settings;

#define WIFI_NAMESPACE          "wifi_cfg"
#define WIFI_KEY_SSID           "ssid"
#define WIFI_KEY_PASS           "pass"
#define WIFI_KEY_BAND           "band"
#define WIFI_KEY_DST            "dst"
#define WIFI_KEY_TZ             "tz"
#define WIFI_KEY_DISP           "disp"
#define WIFI_KEY_BRIGHT         "bright"
#define WIFI_KEY_NIGHT          "night"
#define WIFI_KEY_HOURLY         "hourly"
#define WIFI_KEY_RANGE          "range"
#define WIFI_KEY_NTP_SERVER     "ntp_server"
#define WIFI_KEY_NTP_MODE       "ntp_mode"
#define WIFI_KEY_NTP_URL        "ntp_url"
#define WIFI_KEY_OFFSET         "ofs_tm"
#define WIFI_KEY_IP_MODE        "ip_mode"
#define WIFI_KEY_IP_ADDR        "ip_addr"
#define WIFI_KEY_IP_MASK        "ip_mask"
#define WIFI_KEY_IP_GW          "ip_gw"
#define WIFI_KEY_IP_DNS         "ip_dns"
#define WIFI_KEY_OUTPUT         "output"
#define WIFI_KEY_PPS_TX_ONLY    "pps_tx_only"
#define WIFI_KEY_AP_PASS        "ap_pass"
#define WIFI_KEY_AP_WINDOW      "ap_window"
#define WIFI_KEY_SETUP_STA      "setup_sta"
#define SETUP_AP_WINDOW_MAX_SEC 86400
#define DISPLAY_TIMEOUT_MAX_MIN 1440
#define WIFI_KEY_SCREENSAVER    "screen_sec"
#define WIFI_KEY_SCREEN_TRIGGER "screen_trigger"
#define WIFI_KEY_SCREEN_OFF     "screen_off"

// Empty, or 8-63 printable ASCII characters for WPA2-Personal.
bool settings_ap_password_valid(const char *password);

// Set default configuration values
void settings_set_defaults(app_settings_t *cfg);

// Load settings from NVS
bool load_settings(app_settings_t *cfg);

// Save settings to NVS
esp_err_t save_settings(const app_settings_t *cfg);

// Clear all application settings from their NVS namespace
esp_err_t clear_all_settings(void);

#endif
