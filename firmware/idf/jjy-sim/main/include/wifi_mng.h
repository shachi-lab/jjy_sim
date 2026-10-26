/*
 * JJY-SIM - Wi-Fi Management (API)
 *
 * Provides Wi-Fi connection control and status handling.
 *
 * - Initializes Wi-Fi subsystem
 * - Starts STA connection with given credentials
 * - Provides connection status and IP address
 * - Supports setup mode and reboot request flag
 *
 * Copyright (c) 2026 Shachi-lab
 * SPDX-License-Identifier: MIT
 */
#ifndef _WIFI_MNG_H_
#define _WIFI_MNG_H_
#include "esp_err.h"

typedef enum {
  WIFI_STATUS_UNKNOWN = -1,     // Status not initialized
  WIFI_STATUS_DISCONNECTED = 0, // Not connected
  WIFI_STATUS_CONNECTING,       // Connecting to AP
  WIFI_STATUS_CONNECTED         // Connected and IP acquired
} wifi_status_t;

// Initialize Wi-Fi subsystem (common setup)
void wifi_init_common(void);

// Prepare STA credentials/IP without starting the radio; AP+STA starts together.
void wifi_prepare_sta(const char *ssid, const char *pass);

// Start STA and optionally publish a setup AP for the saved duration.
void wifi_start_setup_window(void);

// Called by the main task: expire the AP, or report a setup-page request.
bool wifi_setup_window_poll(void);

// Read-only status for the OLED's temporary-AP indicator and setup instructions.
bool wifi_is_setup_window_active(void);
const char *wifi_get_ap_ssid(void);

// Enter Wi-Fi setup mode (e.g. AP/captive portal)
void enter_setup_mode(void);

// Physical CONFIG recovery: use an open AP for this boot without changing saved settings.
void enter_setup_recovery_mode(void);

// Returns true if connected to Wi-Fi (wait 1sec)
bool is_wifi_connected(void);

// Get current IP address as string
char *get_wifi_addr_str(void);

// Address shown on the setup screen: STA for a STA-origin session, otherwise AP.
const char *wifi_get_setup_addr_str(void);

// Returns true if reboot is requested
bool is_reboot_req(void);

// Get current Wi-Fi connection status
wifi_status_t wifi_get_status(void);

// OTA worker only: keep the setup AP while temporarily connecting saved STA.
esp_err_t wifi_update_connect(void);
void wifi_update_disconnect(void);

#endif
