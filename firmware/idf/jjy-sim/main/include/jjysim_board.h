/*
 * JJY-SIM - Board Identification (API)
 *
 * Defines hardware generations and read-only board identification APIs.
 *
 * Copyright (c) 2026 Shachi-lab
 * SPDX-License-Identifier: MIT
 */
#pragma once

typedef enum {
    JJYSIM_BOARD_R2 = 2,
    JJYSIM_BOARD_R3 = 3,
    JJYSIM_BOARD_R4 = 4,
} jjysim_board_t;

/* Call before any hardware initialization. No eFuse writes are performed. */
void jjysim_board_init(void);
jjysim_board_t jjysim_board_get(void);
const char *jjysim_board_product_name(void);
