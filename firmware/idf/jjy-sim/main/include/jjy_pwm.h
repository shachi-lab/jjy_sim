/*
 * JJY-SIM - JJY PWM (API)
 *
 * Provides JJY signal generation interface.
 *
 * - Initializes PWM-based JJY output
 * - Generates and transmits time signal per second
 * - Provides timezone string utility
 *
 * Copyright (c) 2026 Shachi-lab
 * SPDX-License-Identifier: MIT
 */
#ifndef JJY_PWM_H
#define JJY_PWM_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  JJY_OUTPUT_LOW = 0,
  JJY_OUTPUT_MID = 1,
  JJY_OUTPUT_HIGH = 2,
} jjy_output_level_t;

// Initialize JJY PWM generator
bool jjy_pwm_init(int band,int dst, float timezone, int offset_time, int output_level);

// Get current time
struct tm *jjy_get_time(void);

// Wait for next second
struct tm *jjw_wait_next_second(char *timeNowStr);

// Process JJY signal output (call once per second)
void jjy_pwm_proc(char *timeNowStr);

// Convert timezone to TZ string
char *jjy_get_tz_str(char *tz, float timezone, int dst);

// Convert day of the week to string
char *jjy_get_week_str(char *buff);

// Set JJY signal output enable
void jjy_set_signal_enable(bool enable);

// Stop any pending symbol and hold the bridge in brake state before setup.
void jjy_pwm_suspend(void);

#endif
