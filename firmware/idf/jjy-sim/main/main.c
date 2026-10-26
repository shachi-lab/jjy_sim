/*
 * JJY-SIM
 *
 * JJY signal simulator using ESP32 / ESP8684.
 *
 * - Generates 40kHz / 60kHz JJY signal
 * - Synchronizes time via NTP over Wi-Fi
 * - Provides OLED display for status and time
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
#include <math.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "driver/i2c_master.h"
#include "esp_app_desc.h"

#include "jjy-sim.h"
#include "jjy_pwm.h"
#include "led.h"
#include "settings.h"
#include "wifi_mng.h"
#include "ntp_server.h"
#include "ota_update.h"

#include "OLEDDisplay.h"
#include "resource/shachi-lab_logo.h"
#include "resource/icon_image.h"
#include "resource/DejaVu_Sans_Condensed_Bold_16.h"
#include "resource/DejaVu_Sans_Condensed_Bold_38.h"

#define DISP_RESET_REASON   0

static const char *TAG __attribute__((unused)) = "main";

static char JJY_char = JJY_CHAR_EAST;     // JJY signal character
static char tz_str[10] = {0};              // timezone string
static char *config_str = "";             // configuration string
static char time_str[40];                 // time string
static struct tm *timeinfo = NULL;        // current time
static bool jjy_enable = true;            // JJY signal enable
static char *ntp_server = NTP_URL;        // NTP server
static char offset_str[8] = {0};          // offset string

typedef enum {
  DISP_REBOOT      = -3,
  DISP_CONFIG_MODE = -2,
  DISP_INIT_LOGO   = 0,
  DISP_VERSION     = 1,
  DISP_CONNECTING  = 2,
  DISP_CONNECTED   = 3,
  DISP_NTP_SYNCING = 4,
  DISP_SENDING     = 5
} disp_screen_t;

static void disp_screen(disp_screen_t mode);
#if DISP_RESET_REASON
static const char *reset_reason_name(esp_reset_reason_t reason);
#endif

// Keep the radio awake through STA association and the first SNTP synchronization.
// When serving NTP, retain this outer guard until the server has stopped so its
// own power-save guard can nest without briefly re-enabling modem sleep.
static wifi_ps_type_t startup_power_save;
static bool startup_awake;

static esp_err_t wifi_startup_awake_begin(void)
{
  if (startup_awake) return ESP_OK;
  esp_err_t err = esp_wifi_get_ps(&startup_power_save);
  if (err == ESP_OK) err = esp_wifi_set_ps(WIFI_PS_NONE);
  if (err == ESP_OK) startup_awake = true;
  else ESP_LOGE(TAG, "Cannot disable startup Wi-Fi power save: %s", esp_err_to_name(err));
  return err;
}

static void wifi_startup_awake_end(void)
{
  if (!startup_awake) return;
  esp_err_t err = esp_wifi_set_ps(startup_power_save);
  if (err == ESP_OK) startup_awake = false;
  else ESP_LOGE(TAG, "Cannot restore startup Wi-Fi power save: %s", esp_err_to_name(err));
}

// SNTPを使って時刻を同期する
static bool wait_for_time_sync(int timeout_ms)
{
  static bool is_initialized = false;

  if (is_initialized == false ) {
    is_initialized = true;
    ESP_LOGI(TAG, "Starting SNTP");

    // SNTP設定
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, ntp_server);
    esp_sntp_set_time_sync_notification_cb(ntp_server_time_synced);
    esp_sntp_init();
  }

  int retry = 0;
  int max_retry = timeout_ms > 0 ? timeout_ms / 500 : 0;

  while (max_retry == 0 || retry < max_retry) {
    if (wifi_setup_window_poll()) return false;

    timeinfo = jjy_get_time();

    // 年が2020未満なら未同期とみなす
    if (timeinfo->tm_year >= 20) {
      sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
      ESP_LOGI(TAG, "Time synchronized");
      printf("Current time: %04d-%02d-%02d %02d:%02d:%02d\n",
                timeinfo->tm_year + 2000,
                timeinfo->tm_mon + 1,
                timeinfo->tm_mday,
                timeinfo->tm_hour,
                timeinfo->tm_min,
                timeinfo->tm_sec);
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    retry++;
  }
  ESP_LOGW(TAG, "SNTP sync timeout");
  return false;
}

// Keep the switch for display/output controls, without a boot countdown.
static void config_switch_init(void)
{
  gpio_config_t io_conf = {
    .pin_bit_mask = (1ULL << PIN_CONFIG),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE
  };
  ESP_ERROR_CHECK(gpio_config(&io_conf));
}

static bool wait_for_setup_recovery(int duration_ms)
{
  static int pressed_ms;
  for (int elapsed = 0; elapsed < duration_ms; elapsed += 10) {
    if (gpio_get_level(PIN_CONFIG) == 0) pressed_ms += 10;
    else pressed_ms = 0;
    if (pressed_ms >= 30) return true;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return false;
}

// Wi-Fi接続状態に応じたアイコンを返して、LEDを制御
static const uint8_t *get_wifi_icon_led(wifi_status_t status, bool ntp_flag)
{ 
  static const uint8_t *icon = NULL;

  switch (status) {
  case WIFI_STATUS_DISCONNECTED :
    icon = wifi_12x12_off_bits;
    led_blink(PIN_LED_WIFI, LED_MODE_BLINK_SLOW);
    break;
  case WIFI_STATUS_CONNECTED :
    icon = wifi_12x12_on_bits;
    led_blink(PIN_LED_WIFI, ntp_flag ? LED_MODE_BLINK_FAST : LED_MODE_ON);
    break;
  case WIFI_STATUS_CONNECTING :
    icon = (icon == wifi_12x12_conn1_bits) ? wifi_12x12_conn2_bits : wifi_12x12_conn1_bits;
    led_blink(PIN_LED_WIFI, LED_MODE_BLINK_SLOW);
    break;
  default :
    icon = NULL;
    led_blink(PIN_LED_WIFI, LED_MODE_OFF);
    break;
  }
  return icon;
}

// I2CベースのOLEDの初期化
static i2c_master_bus_handle_t i2c_bus_init(void)
{
  i2c_master_bus_config_t bus_cfg = {
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .i2c_port = I2C_NUM_0,
    .sda_io_num = PIN_OLED_SDA,
    .scl_io_num = PIN_OLED_SCL,
    .glitch_ignore_cnt = 7,
    .flags.enable_internal_pullup = true,
  };
  i2c_master_bus_handle_t i2c_bus;
  esp_err_t ret = i2c_new_master_bus(&bus_cfg, &i2c_bus);
  if (ret != ESP_OK) {
    return NULL;
  }
  return i2c_bus;
}

static const uint8_t brightness_table[11] = {
  0,    // 0: ほぼ消灯に近い（でも見える）
  1,    // 1 
  5,    // 2
  15,   // 3
  25,   // 4
  45,   // 5
  75,   // 6
  110,  // 7
  150,  // 8
  200,  // 9
  255   // 10: 最大
};

void disp_brightness(int hour)
{
  static int brightness_save = -1;
  int brightness;

  if ((hour >= 0) && s_settings.night_mode && (hour >= NIGHT_TIME_START || hour < NIGHT_TIME_END)) {
    brightness = NIGHT_TIME_BRIGHTNESS;
  } else {
    brightness = s_settings.brightness;
  }
  if (brightness < 0 || brightness > sizeof(brightness_table)-1) {
      brightness = sizeof(brightness_table) - 1;
  }
  if (brightness != brightness_save) {
    brightness_save = brightness;
    OLEDDisplay_setBrightness(brightness_table[brightness]);
  }
}

static void disp_fade(bool direction)
{
  for (uint8_t i = 0; i < sizeof(brightness_table); i++) {
    uint8_t idx = direction ? sizeof(brightness_table) - 1 - i : i;
    OLEDDisplay_setBrightness(brightness_table[idx]);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Test interval; adjust before release.
#define SCREENSAVER_MOVE_INTERVAL_US (5 * 1000000LL)
static bool screensaver_active;
static bool screensaver_interacting;
static bool display_sleeping;
static bool display_timer_started;
static int64_t display_activity_us;
static int64_t screensaver_next_move_us;
static int screensaver_x, screensaver_y;

// Restore the panel only after the new framebuffer has been sent.
static void display_wake(void)
{
  if (display_sleeping) {
    OLEDDisplay_displayOn();
    display_sleeping = false;
  }
}

static void display_activity(void)
{
  display_activity_us = esp_timer_get_time();
  display_timer_started = true;
  screensaver_active = false;
}

static bool draw_screensaver(disp_screen_t mode)
{
  if (mode != DISP_SENDING || timeinfo == NULL) {
    screensaver_active = false;
    display_timer_started = false;
    return false;
  }
  int64_t now = esp_timer_get_time();
  if (s_settings.screensaver_trigger == SCREENSAVER_TRIGGER_JJY_OFF) {
    display_timer_started = false;
    if (jjy_enable || screensaver_interacting) {
      screensaver_active = false;
      return false;
    }
  } else {
    if (s_settings.screensaver_sec == 0) {
      screensaver_active = false;
      display_timer_started = false;
      return false;
    }
    if (!display_timer_started) display_activity();
    if (now - display_activity_us < s_settings.screensaver_sec * 1000000LL) return false;
  }
  if (s_settings.screensaver_off) {
    screensaver_active = true;
    if (!display_sleeping) {
      OLEDDisplay_displayOff();
      display_sleeping = true;
    }
    return true;
  }

  char clock[8];
  snprintf(clock, sizeof(clock), "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
  OLEDDisplay_setFont(ArialMT_Plain_16);
  int max_x = OLEDDisplay_getWidth() - OLEDDisplay_getStringWidth(clock);
  int max_y = OLEDDisplay_getHeight() - ArialMT_Plain_16[1];
  if (!screensaver_active || now >= screensaver_next_move_us) {
    int old_x = screensaver_x, old_y = screensaver_y;
    screensaver_x = esp_random() % (max_x + 1);
    screensaver_y = esp_random() % (max_y + 1);
    if (screensaver_active && screensaver_x == old_x && screensaver_y == old_y && max_x > 0) {
      screensaver_x = (screensaver_x + 1) % (max_x + 1);
    }
    screensaver_next_move_us = now + SCREENSAVER_MOVE_INTERVAL_US;
  }
  // The clock width can change at an hour/minute boundary.
  if (screensaver_x > max_x) screensaver_x = max_x;
  if (screensaver_y > max_y) screensaver_y = max_y;
  screensaver_active = true;
  OLEDDisplay_drawString(screensaver_x, screensaver_y, clock);
  disp_brightness(timeinfo->tm_hour);
  OLEDDisplay_display();
  display_wake();
  return true;
}

// OLEDの画面描画
static void disp_screen(disp_screen_t mode)
{ 
  char buff_str[40];
  static i2c_master_bus_handle_t i2c_bus = NULL;
  int brightness_hour = -1;

  if (i2c_bus == NULL) {
    i2c_bus = i2c_bus_init();
    if (i2c_bus == NULL) return;

    oled_config_t oled_cfg = OLED_CONFIG_DEFAULT();
    oled_cfg.i2c.bus = i2c_bus;
    oled_cfg.rst_pin = PIN_OLED_RST;
    OLEDDisplay_config(&oled_cfg);
    OLEDDisplay_init();
    OLEDDisplay_flipScreenVertically();
  }

  if (mode == DISP_INIT_LOGO) {
    OLEDDisplay_drawBuffer(shachilab_logo_bits, sizeof(shachilab_logo_bits));
    OLEDDisplay_display();
    display_wake();
    return;
  }

  OLEDDisplay_clear();
  OLEDDisplay_setFont(ArialMT_Plain_10);
  if (draw_screensaver(mode)) return;

  OLEDDisplay_drawStringf(0, 0, buff_str, "JJY-SIM R%d", (int)jjysim_board_get());
  if (ota_update_notice()) {
    // Keep the notification one pixel to the right of its former inline position.
    OLEDDisplay_drawString(OLEDDisplay_getStringWidth(buff_str) + 1, 0, "!");
  }

  if (mode == DISP_CONFIG_MODE || (mode >= DISP_CONNECTING && wifi_is_setup_window_active())) {
    OLEDDisplay_drawString(64, 0, "A");
  }
  if (s_settings.hourly_mode) {
    OLEDDisplay_drawString(72, 0, "T");
  }
  if (ntp_server_is_running()) {
    OLEDDisplay_drawString(80, 0, "S");
  }
  // All six status characters use fixed 8-pixel slots: A T S E/W L/M/H X.
  // Avoid proportional-font concatenation compressing the final three slots.
  char status_char[2] = {JJY_char, '\0'};
  OLEDDisplay_drawString(88, 0, status_char);
  status_char[0] = s_settings.output_level == JJY_OUTPUT_LOW ? 'L' :
                   s_settings.output_level == JJY_OUTPUT_MID ? 'M' : 'H';
  OLEDDisplay_drawString(96, 0, status_char);
  // Keep configured band/level visible and mark every non-transmitting state.
  if (mode != DISP_SENDING || !jjy_enable) {
    OLEDDisplay_drawString(104, 0, "X");
  }

  if (mode == DISP_CONFIG_MODE) {
    // A dedicated setup screen must never fall through to the clock/status UI.
    OLEDDisplay_drawString(0, 13, "CONFIG MODE");
    OLEDDisplay_drawString(0, 26, config_str);
    OLEDDisplay_drawStringf(0, 39, buff_str, "AP: %s", wifi_get_ap_ssid());
    OLEDDisplay_drawStringf(0, 51, buff_str, "http://%s/", wifi_get_setup_addr_str());
    disp_brightness(-1);
    OLEDDisplay_display();
    display_wake();
    return;
  }


  const uint8_t *icon = get_wifi_icon_led(wifi_get_status(), mode == DISP_NTP_SYNCING); 
  if (icon)  OLEDDisplay_drawXbm(116, 2, 12, 12, icon);

  do {
    if (mode == DISP_VERSION || mode < DISP_INIT_LOGO) {
      const esp_app_desc_t *app = esp_app_get_description();
      OLEDDisplay_drawStringf( 0, 13, buff_str, "Version %s - " JJYSIM_CHIP_FAMILY, (char*)app->version );
    
      if (mode == DISP_VERSION) {
        OLEDDisplay_drawString( 0, 26, JJYSIM_COPYRIGHT );
        OLEDDisplay_drawString( 0, 38, JJYSIM_VENDOR_NAME );
#if DISP_RESET_REASON
        OLEDDisplay_drawStringf(0, 51, buff_str, "Reset: %s",
                                reset_reason_name(esp_reset_reason()));
#endif
      } else
      if (mode == DISP_REBOOT) {
        OLEDDisplay_drawString( 0, 42, "Reboot now!!" );
      }
      break;
    }
    if (mode < DISP_CONNECTING) break;

    if (mode == DISP_SENDING) {

      brightness_hour = timeinfo->tm_hour;

      if (s_settings.disp_mode == 0) {
        if (timeinfo == NULL) break;
        if (s_settings.dst) {
          OLEDDisplay_drawString(108, 35, "DST");
        }
        time_str[10] = '\0';
        time_str[16] = '\0';
        OLEDDisplay_setFont((uint8_t*)DejaVu_Sans_Condensed_Bold_16);
        OLEDDisplay_drawString(  0, 14, time_str+ 2);
        OLEDDisplay_drawString(109, 49, time_str+17);
        jjy_get_week_str(buff_str);
        OLEDDisplay_setTextAlignment(TEXT_ALIGN_CENTER);
        OLEDDisplay_drawString(110, 14, buff_str+ 0);
        OLEDDisplay_setTextAlignment(TEXT_ALIGN_LEFT);

        OLEDDisplay_setFont((uint8_t*)DejaVu_Sans_Condensed_Bold_38);
        if (time_str[11] == '0') {
          OLEDDisplay_drawString( 23, 28, time_str+12);
        } else {
          OLEDDisplay_drawString( -1, 28, time_str+11);
        }
        break;
      }
    }

    OLEDDisplay_drawStringf( 0, 14, buff_str, "SSID : %s", s_settings.ssid );

    if (mode == DISP_CONNECTING) {
      OLEDDisplay_drawString( 0, 24, "Connecting..." );
    } else {
      OLEDDisplay_drawStringf( 0, 24, buff_str, "Connect : %s", get_wifi_addr_str() );
      if (mode == DISP_NTP_SYNCING) {
        OLEDDisplay_drawString( 0, 34, "NTP SYNCING...");
        OLEDDisplay_drawString( 0, 44, ntp_server);
      } else
      if (mode == DISP_SENDING) {
        OLEDDisplay_drawString( 54, 34, tz_str );
        OLEDDisplay_drawString(110, 34, offset_str );
      if (jjy_enable) {
          OLEDDisplay_drawString( 0, 34, "SEND ON");
        } else {
          OLEDDisplay_drawString( 0, 34, "SEND OFF");
        }
        if (timeinfo == NULL) break;
        OLEDDisplay_setFont(ArialMT_Plain_16);
        OLEDDisplay_drawString( 0, 46, time_str+2);
      }
    }
  } while (0);

  if (mode > DISP_VERSION) {
    disp_brightness(brightness_hour);
  }
  OLEDDisplay_display();
  display_wake();
}

static void config_switch_task(const struct tm *tm_now)
{
  static uint32_t call_count = 0;
  static bool prev_pressed = false;
  static int press_count = 0;
  static bool long_press_done = false;
  static bool manual_jjy = false;

  call_count++;

  bool pressed = (gpio_get_level(PIN_CONFIG) == 0);
  bool wake_only = screensaver_active;
  screensaver_interacting = pressed || prev_pressed;
  if (screensaver_interacting) display_activity();

  if (pressed && !prev_pressed) {
    if (!wake_only) s_settings.disp_mode = !s_settings.disp_mode;
    press_count = 1;
    long_press_done = false;
  } else
  if (pressed && prev_pressed) {
    press_count++;

    if (!long_press_done && press_count >= LONG_PRESS_SEC) {

      jjy_enable = !jjy_enable;
      manual_jjy = true;
      long_press_done = true;
    }
  } else if (!pressed && prev_pressed) {
    press_count = 0;
    long_press_done = false;
  }

  prev_pressed = pressed;

  if (!s_settings.hourly_mode) {
    return;
  }

  if (tm_now->tm_sec != 0) {
    return;
  }

  bool auto_jjy_enable = false;
  int houurly_start = 60 - s_settings.hourly_range;
  int houurly_end = s_settings.hourly_range + 1;

  if (call_count < BOOT_FORCE_ON_COUNT) {
      if (long_press_done) call_count = BOOT_FORCE_ON_COUNT;
      auto_jjy_enable = true;
  } else {
    int min = tm_now->tm_min;
    if (min == houurly_start || min == houurly_end) {
      manual_jjy = false;
    }
    auto_jjy_enable = (min >= houurly_start || min < houurly_end);
  }
  if (manual_jjy == false) {
    jjy_enable = auto_jjy_enable;
  }
}

void band_sel(bool west)
{
  if (jjysim_board_get() != JJYSIM_BOARD_R4) return;

  static bool initialized = false; 
  
  if(initialized == false) {
    gpio_reset_pin(PIN_BAND_SEL);
    gpio_set_drive_capability(PIN_BAND_SEL, GPIO_DRIVE_CAP_3);
    gpio_set_direction(PIN_BAND_SEL, GPIO_MODE_OUTPUT);
    initialized = true;
  }
  gpio_set_level(PIN_BAND_SEL, west ? 0 : 1);
}

#if DISP_RESET_REASON
// Keep reset diagnostics visible independently of the configured log level.
static const char *reset_reason_name(esp_reset_reason_t reason)
{
  switch (reason) {
  case ESP_RST_POWERON: return "POWERON";
  case ESP_RST_EXT: return "EXT";
  case ESP_RST_SW: return "SW";
  case ESP_RST_PANIC: return "PANIC";
  case ESP_RST_INT_WDT: return "INT_WDT";
  case ESP_RST_TASK_WDT: return "TASK_WDT";
  case ESP_RST_WDT: return "WDT";
  case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
  case ESP_RST_BROWNOUT: return "BROWNOUT";
  case ESP_RST_SDIO: return "SDIO";
  case ESP_RST_USB: return "USB";
  case ESP_RST_JTAG: return "JTAG";
  case ESP_RST_EFUSE: return "EFUSE";
  case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
  case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
  default: return "UNKNOWN";
  }
}
#endif

// main
void app_main(void)
{
  jjysim_board_init();
  led_init();
  led_blink(PIN_LED_WIFI, LED_MODE_ON);
  led_blink(PIN_LED_PPS , LED_MODE_ON);

  const esp_app_desc_t *app = esp_app_get_description();

  printf("\n%s", JJYSIM_PRODUCT_NAME);
  printf("\n%s", app->version);
  printf("\n%s", JJYSIM_COPYRIGHT " " JJYSIM_VENDOR_NAME "\n");
#if DISP_RESET_REASON
  esp_reset_reason_t reset_reason = esp_reset_reason();
  printf("Reset reason: %s (%d)\n", reset_reason_name(reset_reason), (int)reset_reason);
  fflush(stdout);
#endif
  disp_screen(DISP_INIT_LOGO);
  disp_fade(false);

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  load_settings(&s_settings);
  ota_update_init();

  if (s_settings.band == JJY_BAND_WEST) {
    JJY_char = JJY_CHAR_WEST;
    band_sel( true );
  } else {
    JJY_char = JJY_CHAR_EAST;
    band_sel( false );
  }

  jjy_get_tz_str(tz_str, s_settings.timezone, s_settings.dst);
  if (s_settings.dst) strcat(tz_str, "*");
  sprintf(offset_str, "%+d", s_settings.offset_time);
  if (s_settings.ntp_mode && s_settings.ntp_url[0] != '\0') {    
    ntp_server = s_settings.ntp_url;
  } else {
    ntp_server = NTP_URL;
  }

  vTaskDelay(pdMS_TO_TICKS(2000));
  disp_fade(true);

  led_blink(PIN_LED_WIFI, LED_MODE_OFF);
  led_blink(PIN_LED_PPS , LED_MODE_OFF);

  config_switch_init();
  disp_screen(DISP_VERSION);
  bool setup_recovery = false;
  for (uint8_t i = 0; i < sizeof(brightness_table) && !setup_recovery; i++) {
    OLEDDisplay_setBrightness(brightness_table[i]);
    setup_recovery = wait_for_setup_recovery(50);
  }
  if (!setup_recovery) setup_recovery = wait_for_setup_recovery(2000);
  // A CONFIG press can interrupt the fade while the display is still dark.
  if (setup_recovery) OLEDDisplay_setBrightness(brightness_table[sizeof(brightness_table) - 1]);

  wifi_init_common();
  if (setup_recovery) {
    config_str = "Setup recovery";
    goto setup;
  }

  ESP_LOGI(TAG, "Wi-Fi Valid : %d", s_settings.wifi_valid);
  if (s_settings.wifi_valid) {
    if (wifi_startup_awake_begin() != ESP_OK) {
      config_str = "Error: WiFi power";
      goto setup;
    }
    wifi_prepare_sta(s_settings.ssid, s_settings.pass);
    wifi_start_setup_window();
    for (uint8_t i = 0; i < CONNECT_TIMEOUT; i++) {
      if (wifi_setup_window_poll()) goto setup_requested;
      disp_screen(DISP_CONNECTING);
      if (is_wifi_connected()) break;
    }

    if (wifi_setup_window_poll()) goto setup_requested;
    if (wifi_get_status() != WIFI_STATUS_CONNECTED) {
      config_str = "Error: WiFi timeout";
    } else {
      disp_screen(DISP_NTP_SYNCING);
      bool synced = wait_for_time_sync(NTP_TIMEOUT * 1000);
      if (wifi_setup_window_poll()) goto setup_requested;
      if (!synced) {
        config_str = "Error: NTP timeout";
      } else if (!jjy_pwm_init(s_settings.band, s_settings.dst, s_settings.timezone,
                               s_settings.offset_time, s_settings.output_level)) {
        config_str = "Error: PWM init";
      } else {
        if (wifi_setup_window_poll()) goto setup_requested;
        // Normal operation is ready: time sync and carrier initialization passed.
        ota_update_confirm_boot();
        if (s_settings.ntp_server_enabled) {
          esp_err_t err = ntp_server_start();
          if (err != ESP_OK) ESP_LOGE(TAG, "NTP server start failed: %s", esp_err_to_name(err));
        } else {
          wifi_startup_awake_end();
        }
        while (!wifi_setup_window_poll()) {
          timeinfo = jjw_wait_next_second(time_str);
          if (wifi_setup_window_poll()) break;
          config_switch_task(timeinfo);
          jjy_set_signal_enable(jjy_enable);
          jjy_pwm_proc(time_str);
          ota_update_poll();
          disp_screen(DISP_SENDING);
        }
        config_str = "Transmission stopped";
      }
    }
  } else {
    config_str = "SSID is not set";
  }

  goto setup;

setup_requested:
  config_str = "Setup requested";
setup:
  // Stop the carrier before acknowledging the portal request or allowing edits.
  jjy_pwm_suspend();
  ntp_server_stop();
  wifi_startup_awake_end();
  esp_sntp_stop();
  jjy_enable = false;
  ESP_LOGI(TAG, "%s", config_str);
  ESP_LOGI(TAG, "Enter to CONFIG mode");
  disp_screen(DISP_CONFIG_MODE);
  led_blink(PIN_LED_WIFI, LED_MODE_OFF);
  led_blink(PIN_LED_PPS , LED_MODE_ON);
  if (setup_recovery) enter_setup_recovery_mode();
  else enter_setup_mode();
  // Offline operation is healthy too, once the recovery/settings portal is ready.
  ota_update_confirm_boot();
  disp_screen(DISP_CONFIG_MODE);

  bool displayed_notice = ota_update_notice();
  char displayed_setup_addr[16];
  strlcpy(displayed_setup_addr, wifi_get_setup_addr_str(), sizeof(displayed_setup_addr));
  while (is_reboot_req() == false) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    bool current_notice = ota_update_notice();
    if (current_notice != displayed_notice ||
        strcmp(displayed_setup_addr, wifi_get_setup_addr_str()) != 0) {
      disp_screen(DISP_CONFIG_MODE);
      displayed_notice = current_notice;
      strlcpy(displayed_setup_addr, wifi_get_setup_addr_str(), sizeof(displayed_setup_addr));
    }
  }

  disp_screen(DISP_REBOOT);
  led_blink(PIN_LED_WIFI, LED_MODE_BLINK_FAST);
  led_blink(PIN_LED_PPS , LED_MODE_BLINK_FAST);

  vTaskDelay(pdMS_TO_TICKS(2000));
  esp_restart();
}
