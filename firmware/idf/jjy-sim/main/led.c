/*
 * JJY-SIM - LED Control
 *
 * Controls status LEDs for Wi-Fi and PPS indication.
 *
 * - Supports blink mode using LEDC (hardware PWM)
 * - Supports short/long pulse using esp_timer
 * - Used for JJY signal timing and status indication
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
#include "esp_timer.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_check.h"

#include "jjy-sim.h"
#include "led.h"

static const char *TAG __attribute__((unused)) = "led";

#define LED_BLINK_FREQ_SLOW     4           // 4Hz
#define LED_BLINK_FREQ_FAST     8           // 8Hz
#define LED_PULSE_S_TIME_US     20000       // 20ms
#define LED_PULSE_L_TIME_US     200000      // 200ms 

#define LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_TIMER_SLOW LEDC_TIMER_2
#define LEDC_TIMER_FAST LEDC_TIMER_3
#define LEDC_FREQ_SLOW  LED_BLINK_FREQ_SLOW
#define LEDC_FREQ_FAST  LED_BLINK_FREQ_FAST
#define LEDC_RES        LEDC_TIMER_14_BIT
#define LED_DUTY        (1 << LEDC_RES) / 2
#define LEDC_CH_WIFI    LEDC_CHANNEL_2
#define LEDC_CH_PPS     LEDC_CHANNEL_3

static esp_timer_handle_t s_led_off_timer = NULL;
static int led_pulse_pin  = -1; 

static void led_off_timer_cb(void *arg)
{
  if (led_pulse_pin >= 0) {
    gpio_set_level(led_pulse_pin, LED_LEVEL_OFF);
    led_pulse_pin = -1;
  }
}

static void led_pulse_init_once(void)
{
  if (s_led_off_timer != NULL)    return;

  esp_timer_create_args_t timer_args = {
    .callback = led_off_timer_cb,
    .arg = NULL,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "led_off"
  };

  esp_timer_create(&timer_args, &s_led_off_timer);
}

static void led_pulse_us(int led_pin, uint32_t on_time_us)
{
  led_pulse_pin = led_pin;
  gpio_set_level(led_pin, LED_LEVEL_ON);

  led_pulse_init_once();
  esp_timer_stop(s_led_off_timer);  // 動いてたら止める
  esp_timer_start_once(s_led_off_timer, on_time_us);
}

static void ledc_init_once(void)
{
  static bool initialized = false;
  if (initialized) return;

  ledc_timer_config_t t = {
    .speed_mode = LEDC_MODE,
    .timer_num = LEDC_TIMER_SLOW,
    .duty_resolution = LEDC_RES,
    .freq_hz = LEDC_FREQ_SLOW,
    .clk_cfg = LEDC_USE_XTAL_CLK,
  };
  ledc_timer_config(&t);

  t.timer_num = LEDC_TIMER_FAST;
  t.freq_hz = LEDC_FREQ_FAST;
  ledc_timer_config(&t);

  initialized = true;
}

void led_init(void)
{
  gpio_reset_pin(PIN_LED_WIFI);
  gpio_reset_pin(PIN_LED_PPS);
  gpio_reset_pin(PIN_LED_RGB);
  gpio_set_direction(PIN_LED_WIFI, GPIO_MODE_OUTPUT);
  gpio_set_direction(PIN_LED_PPS, GPIO_MODE_OUTPUT);
  gpio_set_direction(PIN_LED_RGB, GPIO_MODE_OUTPUT);
  gpio_set_level(PIN_LED_WIFI, LED_LEVEL_OFF);
  gpio_set_level(PIN_LED_PPS, LED_LEVEL_OFF);
  gpio_set_level(PIN_LED_RGB, 0);
}

void led_blink(uint8_t pin, led_mode_t mode)
{
  static uint8_t s_attached[LEDC_CHANNEL_MAX] = {0};

  int led_ch;
  if (pin == PIN_LED_WIFI) {
    led_ch = LEDC_CH_WIFI;
  } else
  if (pin == PIN_LED_PPS ) {
    led_ch = LEDC_CH_PPS;
  } else {     
    return;
  }

  if (mode == LED_MODE_BLINK_FAST || mode == LED_MODE_BLINK_SLOW) {
    if (s_attached[led_ch] == mode) return;
    if (s_attached[led_ch]) {
      gpio_reset_pin(pin);
    }
    ledc_init_once();
    ledc_channel_config_t conf = {
      .gpio_num = pin,
      .speed_mode = LEDC_MODE,
      .channel = led_ch,
      .timer_sel = (mode == LED_MODE_BLINK_FAST) ? LEDC_TIMER_FAST : LEDC_TIMER_SLOW,
      .duty = LED_DUTY,
      .hpoint = 0,
    };
    ledc_channel_config(&conf);
    s_attached[led_ch] = mode;
    return;
  }

  if (s_attached[led_ch]) {
    ledc_stop(LEDC_MODE, led_ch, 0);
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    s_attached[led_ch] = 0;
  }

  if (mode == LED_MODE_PULSE_SHORT) {
    led_pulse_us(pin, LED_PULSE_S_TIME_US);
  } else
  if (mode == LED_MODE_PULSE_LONG) {
    led_pulse_us(pin, LED_PULSE_L_TIME_US);
  } else {
    gpio_set_level(pin, mode); 
  }
}
