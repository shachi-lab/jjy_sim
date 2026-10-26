/*
 * JJY-SIM - NTP Server (API)
 *
 * Declares STA-side NTP server lifecycle and synchronization APIs.
 *
 * Copyright (c) 2026 Shachi-lab
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <sys/time.h>
#include "esp_err.h"

// Register with SNTP before initialization, even when serving is disabled.
void ntp_server_time_synced(struct timeval *time);
// Called by the main task only. Serving is limited to the STA interface.
esp_err_t ntp_server_start(void);
void ntp_server_stop(void);
// True only after a successful STA socket bind; false on disconnect/error/stop.
bool ntp_server_is_running(void);
