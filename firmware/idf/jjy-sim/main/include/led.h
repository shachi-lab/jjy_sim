/*
 * JJY-SIM - LED Control (API)
 *
 * Provides simple LED control functions.
 *
 * - Supports ON/OFF, blink, and pulse modes
 * - Used for Wi-Fi status and PPS indication
 *
 * Copyright (c) 2026 Shachi-lab
 * SPDX-License-Identifier: MIT
 */
#ifndef _LED_H_
#define _LED_H_

#include "driver/gpio.h"
#include "esp_err.h"

#define LED_LEVEL_ON    0
#define LED_LEVEL_OFF   1

// LED control modes
typedef enum {
  LED_MODE_ON         = 0,
  LED_MODE_OFF        = 1,
  LED_MODE_BLINK_FAST = 2,
  LED_MODE_BLINK_SLOW = 3,
  LED_MODE_PULSE_LONG = 4,
  LED_MODE_PULSE_SHORT= 5
} led_mode_t;

// Initialize LED GPIOs
void led_init(void);

// Control LED behavior (ON/OFF, blink, pulse)
void led_blink(uint8_t pin, led_mode_t mode);

#endif
