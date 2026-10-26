/*
 * JJY-SIM - Board Identification
 *
 * Identifies the R2/R3/R4 hardware generation.
 * - Reads the hardware-generation eFuse on ESP32-C3
 * - Selects legacy defaults when identification is unavailable
 * - Does not write eFuses
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
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "jjysim_board.h"

#if CONFIG_IDF_TARGET_ESP32C3
#if !CONFIG_EFUSE_CUSTOM_TABLE
#error "ESP32-C3 hardware identification requires CONFIG_EFUSE_CUSTOM_TABLE"
#endif
#include "esp_efuse.h"
#include "esp_efuse_custom_table.h"
#endif

#if CONFIG_IDF_TARGET_ESP32C3
#define JJYSIM_LEGACY_BOARD JJYSIM_BOARD_R2
#else
#define JJYSIM_LEGACY_BOARD JJYSIM_BOARD_R3
#endif

static jjysim_board_t board = JJYSIM_LEGACY_BOARD;

void jjysim_board_init(void)
{
    board = JJYSIM_LEGACY_BOARD;
#if CONFIG_IDF_TARGET_ESP32C3
    uint8_t generation = 0;
    esp_err_t err = esp_efuse_read_field_blob(
        ESP_EFUSE_USER_DATA_JJYSIM_HW_GENERATION, &generation, 8);
    if (err != ESP_OK) {
        ESP_LOGW("board", "eFuse read failed (%s); using legacy/R2", esp_err_to_name(err));
    } else if (generation == JJYSIM_BOARD_R4) {
        board = JJYSIM_BOARD_R4;
    } else if (generation != 0) {
        ESP_LOGW("board", "Unknown eFuse generation 0x%02x; using legacy/R2", generation);
    }
#endif
    ESP_LOGI("board", "Hardware: R%d", (int)board);
}

jjysim_board_t jjysim_board_get(void)
{
    return board;
}

const char *jjysim_board_product_name(void)
{
    switch (board) {
    case JJYSIM_BOARD_R4: return "JJY Simulator R4";
    case JJYSIM_BOARD_R3: return "JJY Simulator R3";
    default: return "JJY Simulator R2";
    }
}
