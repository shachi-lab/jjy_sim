/*
 * JJY-SIM - Settings Management
 *
 * Handles application settings stored in NVS.
 *
 * - Loads and saves configuration (Wi-Fi, band, timezone, etc.)
 * - Encrypts SSID and password before storage
 * - Provides default initialization and full settings reset
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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "jjy-sim.h"
#include "settings.h"
#include "storage_crypto.h"

static const char *TAG __attribute__((unused)) = "settings";

app_settings_t s_settings;

bool settings_ap_password_valid(const char *password)
{
  size_t len = strlen(password);
  if (len == 0) return true;
  if (len < 8 || len > 63) return false;
  for (size_t i = 0; i < len; i++) {
    if ((unsigned char)password[i] < 32 || (unsigned char)password[i] > 126) return false;
  }
  return true;
}

void settings_set_defaults(app_settings_t *cfg)
{
  memset(cfg, 0, sizeof(*cfg));
  cfg->band = JJY_BAND_EAST;
  cfg->timezone = TIMEZONE_JAPAN;
  cfg->dst = false;
  cfg->hourly_mode = false,
  cfg->hourly_range = HOURLY_RANGE_DEFAULT;
  cfg->wifi_valid = false;
  cfg->disp_mode = 0;
  cfg->brightness = BRIGHTNESS_MAX;
  cfg->night_mode = false;
  cfg->ntp_mode = false;
  cfg->offset_time = 0;
  cfg->output_level = JJY_OUTPUT_HIGH;
  cfg->setup_ap_window_sec = SETUP_AP_WINDOW_SEC;
  cfg->setup_sta_enabled = false;
}

bool load_settings(app_settings_t *cfg)
{
  settings_set_defaults(cfg);

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) {
    return false;
  }

  storage_crypto_blob_t ssid_blob = {0};
  size_t len = sizeof(ssid_blob);
  err = nvs_get_blob(nvs, WIFI_KEY_SSID, &ssid_blob, &len);
  if (err == ESP_OK && len == sizeof(ssid_blob) &&
      storage_decrypt(&ssid_blob, cfg->ssid, sizeof(cfg->ssid)) && cfg->ssid[0] != '\0') {
    cfg->wifi_valid = true;
  } else {
    cfg->ssid[0] = '\0';
  }

  storage_crypto_blob_t pass_blob = {0};
  len = sizeof(pass_blob);
  err = nvs_get_blob(nvs, WIFI_KEY_PASS, &pass_blob, &len);
  if (err != ESP_OK || len != sizeof(pass_blob) || pass_blob.len == 0 ||
      !storage_decrypt(&pass_blob, cfg->pass, sizeof(cfg->pass))) {
    cfg->pass[0] = '\0';
  }

  int32_t v32;
  if (nvs_get_i32(nvs, WIFI_KEY_BAND, &v32) == ESP_OK) {
    cfg->band = (int)v32;
  }
  if (nvs_get_i32(nvs, WIFI_KEY_DST, &v32) == ESP_OK) {
    cfg->dst = (int)v32;
  }
  if (nvs_get_i32(nvs, WIFI_KEY_HOURLY, &v32) == ESP_OK) {
    cfg->hourly_mode = (int)v32;
  }
  if (nvs_get_i32(nvs, WIFI_KEY_RANGE, &v32) == ESP_OK) {
    cfg->hourly_range = (int)v32;
  }
  if (nvs_get_i32(nvs, WIFI_KEY_OFFSET, &v32) == ESP_OK) {
    cfg->offset_time = (int)v32;
  }

  char tzbuf[16];
  len = sizeof(tzbuf);
  if (nvs_get_str(nvs, WIFI_KEY_TZ, tzbuf, &len) == ESP_OK) {
    cfg->timezone = strtof(tzbuf, NULL);
  }
  if (nvs_get_i32(nvs, WIFI_KEY_DISP, &v32) == ESP_OK) {
    cfg->disp_mode = (int)v32;
  }
  if (nvs_get_i32(nvs, WIFI_KEY_BRIGHT, &v32) == ESP_OK) {
    cfg->brightness = (int)v32;
  }
  if (nvs_get_i32(nvs, WIFI_KEY_NIGHT, &v32) == ESP_OK) {
    cfg->night_mode = (int)v32;
  }

  if (nvs_get_i32(nvs, WIFI_KEY_NTP_MODE, &v32) == ESP_OK) {
    cfg->ntp_mode = (int)v32;
  }
  if (nvs_get_i32(nvs, WIFI_KEY_NTP_SERVER, &v32) == ESP_OK) {
    cfg->ntp_server_enabled = (v32 == 1);
  }
  len = sizeof(cfg->ntp_url);
  if (nvs_get_str(nvs, WIFI_KEY_NTP_URL, cfg->ntp_url, &len) != ESP_OK) {
    cfg->ntp_url[0] = '\0';
  }

  if (nvs_get_i32(nvs, WIFI_KEY_IP_MODE, &v32) == ESP_OK) {
    cfg->wifi_static_ip = (v32 == 1);
  }
  len = sizeof(cfg->wifi_ip);
  if (nvs_get_str(nvs, WIFI_KEY_IP_ADDR, cfg->wifi_ip, &len) != ESP_OK) cfg->wifi_ip[0] = '\0';
  len = sizeof(cfg->wifi_netmask);
  if (nvs_get_str(nvs, WIFI_KEY_IP_MASK, cfg->wifi_netmask, &len) != ESP_OK) cfg->wifi_netmask[0] = '\0';
  len = sizeof(cfg->wifi_gateway);
  if (nvs_get_str(nvs, WIFI_KEY_IP_GW, cfg->wifi_gateway, &len) != ESP_OK) cfg->wifi_gateway[0] = '\0';
  len = sizeof(cfg->wifi_dns);
  if (nvs_get_str(nvs, WIFI_KEY_IP_DNS, cfg->wifi_dns, &len) != ESP_OK) cfg->wifi_dns[0] = '\0';

  if (nvs_get_i32(nvs, WIFI_KEY_OUTPUT, &v32) == ESP_OK &&
      v32 >= JJY_OUTPUT_LOW && v32 <= JJY_OUTPUT_HIGH) cfg->output_level = v32;

  if (nvs_get_i32(nvs, WIFI_KEY_PPS_TX_ONLY, &v32) == ESP_OK) {
    cfg->pps_tx_only = (v32 == 1);
  }

  storage_crypto_blob_t ap_pass_blob = {0};
  len = sizeof(ap_pass_blob);
  if (nvs_get_blob(nvs, WIFI_KEY_AP_PASS, &ap_pass_blob, &len) == ESP_OK &&
      len == sizeof(ap_pass_blob) && ap_pass_blob.len > 0) {
    if (!storage_decrypt(&ap_pass_blob, cfg->setup_ap_pass, sizeof(cfg->setup_ap_pass)) ||
        !settings_ap_password_valid(cfg->setup_ap_pass)) cfg->setup_ap_pass[0] = '\0';
  }
  if (nvs_get_i32(nvs, WIFI_KEY_AP_WINDOW, &v32) == ESP_OK &&
      v32 >= 0 && v32 <= SETUP_AP_WINDOW_MAX_SEC) cfg->setup_ap_window_sec = v32;
  if (nvs_get_i32(nvs, WIFI_KEY_SCREENSAVER, &v32) == ESP_OK &&
      v32 >= 0 && v32 <= DISPLAY_TIMEOUT_MAX_MIN * 60) cfg->screensaver_sec = v32;
  if (nvs_get_i32(nvs, WIFI_KEY_SCREEN_TRIGGER, &v32) == ESP_OK &&
      (v32 == SCREENSAVER_TRIGGER_TIMEOUT || v32 == SCREENSAVER_TRIGGER_JJY_OFF)) {
    cfg->screensaver_trigger = (screensaver_trigger_t)v32;
  }
  if (nvs_get_i32(nvs, WIFI_KEY_SCREEN_OFF, &v32) == ESP_OK) cfg->screensaver_off = (v32 == 1);
  if (nvs_get_i32(nvs, WIFI_KEY_SETUP_STA, &v32) == ESP_OK) cfg->setup_sta_enabled = (v32 == 1);

  nvs_close(nvs);
  return true;
}

esp_err_t save_settings(const app_settings_t *cfg)
{
  if (!settings_ap_password_valid(cfg->setup_ap_pass) ||
      cfg->setup_ap_window_sec < 0 || cfg->setup_ap_window_sec > SETUP_AP_WINDOW_MAX_SEC ||
      cfg->screensaver_sec < 0 || cfg->screensaver_sec > DISPLAY_TIMEOUT_MAX_MIN * 60 ||
      (cfg->screensaver_trigger != SCREENSAVER_TRIGGER_TIMEOUT &&
       cfg->screensaver_trigger != SCREENSAVER_TRIGGER_JJY_OFF)) {
    return ESP_ERR_INVALID_ARG;
  }
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    return err;
  }

  storage_crypto_blob_t ssid_blob = {0};
  storage_crypto_blob_t pass_blob = {0};
  storage_crypto_blob_t ap_pass_blob = {0};
  // An all-zero password blob represents an open network.
  if (!storage_encrypt(cfg->ssid, &ssid_blob) ||
      (cfg->pass[0] != '\0' && !storage_encrypt(cfg->pass, &pass_blob)) ||
      (cfg->setup_ap_pass[0] != '\0' && !storage_encrypt(cfg->setup_ap_pass, &ap_pass_blob))) {
    nvs_close(nvs);
    return ESP_FAIL;
  }

  err = nvs_set_blob(nvs, WIFI_KEY_SSID, &ssid_blob, sizeof(ssid_blob));
  if (err == ESP_OK) err = nvs_set_blob(nvs, WIFI_KEY_PASS, &pass_blob, sizeof(pass_blob));
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_BAND, cfg->band);
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_DST, cfg->dst);
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_HOURLY, cfg->hourly_mode);
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_RANGE, cfg->hourly_range);
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_OFFSET, cfg->offset_time);
  if (err == ESP_OK) {
    char tzbuf[16];
    snprintf(tzbuf, sizeof(tzbuf), "%.2f", cfg->timezone);
    err = nvs_set_str(nvs, WIFI_KEY_TZ, tzbuf);
  }
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_DISP, cfg->disp_mode);
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_BRIGHT, cfg->brightness);
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_NIGHT, cfg->night_mode);
  
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_NTP_MODE, cfg->ntp_mode);
  if (err == ESP_OK) err = nvs_set_str(nvs, WIFI_KEY_NTP_URL, cfg->ntp_url);
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_NTP_SERVER, cfg->ntp_server_enabled);

  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_IP_MODE, cfg->wifi_static_ip);
  if (err == ESP_OK) err = nvs_set_str(nvs, WIFI_KEY_IP_ADDR, cfg->wifi_ip);
  if (err == ESP_OK) err = nvs_set_str(nvs, WIFI_KEY_IP_MASK, cfg->wifi_netmask);
  if (err == ESP_OK) err = nvs_set_str(nvs, WIFI_KEY_IP_GW, cfg->wifi_gateway);
  if (err == ESP_OK) err = nvs_set_str(nvs, WIFI_KEY_IP_DNS, cfg->wifi_dns);
  
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_OUTPUT, cfg->output_level);
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_PPS_TX_ONLY, cfg->pps_tx_only);
  if (err == ESP_OK) err = nvs_set_blob(nvs, WIFI_KEY_AP_PASS, &ap_pass_blob, sizeof(ap_pass_blob));
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_AP_WINDOW, cfg->setup_ap_window_sec);
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_SETUP_STA, cfg->setup_sta_enabled);
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_SCREENSAVER, cfg->screensaver_sec);
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_SCREEN_OFF, cfg->screensaver_off);
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_SCREEN_TRIGGER, cfg->screensaver_trigger);
  if (err == ESP_OK) err = nvs_commit(nvs);
  
  nvs_close(nvs);
  return err;
}

esp_err_t clear_all_settings(void)
{
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) return err;
  // All application settings share this namespace; keep other NVS data intact.
  err = nvs_erase_all(nvs);
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  return err;
}
