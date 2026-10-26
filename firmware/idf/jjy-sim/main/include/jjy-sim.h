/*
 * JJY-SIM - Hardware / Configuration Definitions
 *
 * Defines hardware pin assignments and global constants
 * for JJY-SIM.
 *
 * - GPIO assignments (LED, PWM, OLED, etc.)
 * - JJY band definitions (40kHz / 60kHz)
 * - Product information and timing constants
 *
 * Copyright (c) 2026 Shachi-lab
 * SPDX-License-Identifier: MIT
 */
#ifndef _JJY_SIM_H_
#define _JJY_SIM_H_

#include "jjysim_board.h"

// === GPIO assignments ===
#define PIN_CONFIG              GPIO_NUM_9
#define PIN_LED_RGB             GPIO_NUM_2
#define PIN_LED_WIFI            GPIO_NUM_5
#define PIN_LED_PPS             GPIO_NUM_0
#define PIN_PWM_A               GPIO_NUM_10
#define PIN_PWM_B               GPIO_NUM_4
#define PIN_OLED_RST            GPIO_NUM_8
#define PIN_OLED_SCL            GPIO_NUM_6
#define PIN_OLED_SDA            GPIO_NUM_7
#define PIN_BAND_SEL            GPIO_NUM_3

// === JJY configuration ===
#define JJY_CHAR_EAST           'E'
#define JJY_CHAR_WEST           'W'
#define JJY_BAND_EAST           40
#define JJY_BAND_WEST           60

// === Product info ===
#define JJYSIM_VENDOR_NAME      "Shachi-lab"
#define JJYSIM_PRODUCT_NAME     jjysim_board_product_name()
#define JJYSIM_COPYRIGHT        "Copyright (c) 2026"
#define JJYSIM_LICENSE_NAME     "MIT"
#define JJYSIM_LICENSE_URL      "https://opensource.org/licenses/MIT"

#if CONFIG_IDF_TARGET_ESP32C3
#define JJYSIM_MODULE_NAME      "ESP32-C3-WROOM-02"
#define JJYSIM_CERTIFICATION    "R201-220555"
#define JJYSIM_CHIP_NAME        "ESP32-C3"
#define JJYSIM_CHIP_FAMILY      "C3"
#elif CONFIG_IDF_TARGET_ESP32C2
#define JJYSIM_MODULE_NAME      "ESP8684-WROOM-02C"
#define JJYSIM_CERTIFICATION    "R020-240412"
#define JJYSIM_CHIP_NAME        "ESP8684"
#define JJYSIM_CHIP_FAMILY      "C2"
#else
#define JJYSIM_MODULE_NAME      "Unknown"
#define JJYSIM_GITEKIN          "Unknown"
#define JJYSIM_CHIP_NAME        "Unknown"
#define JJYSIM_CHIP_FAMILY      "--"
#endif 

// === Timing ===
#define SETUP_AP_WINDOW_SEC     180     // from normal operation start
#define CONNECT_TIMEOUT         30      // sec

#define NTP_TIMEOUT             30      // sec
#define NTP_URL                 "ntp.nict.jp"   // 日本ならこれが安定
#define TIMEZONE_JAPAN          9.0     // JST

#define NIGHT_TIME_START        22      // 22:00
#define NIGHT_TIME_END          7       // 07:00
#define NIGHT_TIME_BRIGHTNESS   0       // 0
#define BRIGHTNESS_MAX          10

#define HOURLY_RANGE_DEFAULT    5       // 5 min (00:55～05:59)

#define LONG_PRESS_SEC          5
#define BOOT_FORCE_ON_COUNT     600

#endif