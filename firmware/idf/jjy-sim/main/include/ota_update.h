/*
 * JJY-SIM - HTTPS OTA Update (API)
 *
 * Declares update checking, installation, notification and Web APIs.
 *
 * Copyright (c) 2026 Shachi-lab
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include "esp_http_server.h"
bool ota_update_busy(void);
// Initialize after NVS, then poll only from normal operation (never blocks on I/O).
void ota_update_init(void);
void ota_update_poll(void);
bool ota_update_notice(void);
esp_err_t ota_update_page(httpd_req_t *req);
esp_err_t ota_update_status(httpd_req_t *req);
esp_err_t ota_update_check(httpd_req_t *req);
esp_err_t ota_update_start(httpd_req_t *req);
void ota_update_confirm_boot(void);
