// ============================================================================
//  DeskDash — a swipeable desk dashboard for the
//  Waveshare ESP32-C5-Touch-LCD-3.5 (320 x 480, ST7796 + FT6336)
//
//  Pages (swipe left/right):
//    1. Clock    — time, date, indoor temperature/humidity, current weather
//    2. Weather  — conditions, high/low, feels-like, humidity, wind, next 4 hours
//    3. Arrivals — live flights arriving at PBI (adsb.lol ADS-B data)
//    4. Trending — top trending topics on X (scraped from trends24.in)
//    5. System   — Wi-Fi, battery, uptime, brightness slider
//
//  Weather comes from Open-Meteo (free, no API key). Time comes from NTP.
//  Set your Wi-Fi and location in config.h.
// ============================================================================
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_heap_caps.h>
#include "src/lvgl/lvgl.h"     // LVGL 8.4 is bundled in src/ (config in src/lv_conf.h)

#include "config.h"

// Defaults for settings added after the first release, so older config.h files still build
#ifndef TRENDS_LOCATION
#define TRENDS_LOCATION         "united-states"
#define TRENDS_LOCATION_NAME    "United States"
#endif
#ifndef TRENDS_REFRESH_MINUTES
#define TRENDS_REFRESH_MINUTES  30
#endif
#ifndef TRENDS_COUNT
#define TRENDS_COUNT            10
#endif
#ifndef AIRPORT_ICAO
#define AIRPORT_ICAO            "KPBI"
#define AIRPORT_CODE            "PBI"
#define AIRPORT_NAME            "Palm Beach Intl"
#define AIRPORT_LAT             26.6832
#define AIRPORT_LON             -80.0956
#endif
#ifndef ARRIVALS_RADIUS_NM
#define ARRIVALS_RADIUS_NM      60
#endif
#ifndef ARRIVALS_REFRESH_SECONDS
#define ARRIVALS_REFRESH_SECONDS 240    // adsb.lol allows ~500 requests/day per address
#endif
#ifndef ARRIVALS_NIGHT_REFRESH_SECONDS
#define ARRIVALS_NIGHT_REFRESH_SECONDS 900
#endif
#include "Board_IO.h"
#include "Touch_FT6336.h"
#include "lcd_st7796.h"
#include "sensors.h"
#include "flights.h"

// ---- Fonts generated for this project (see the font_*.c files) --------------
LV_FONT_DECLARE(font_clock_104);  // digits and colon for the big clock
LV_FONT_DECLARE(font_text_56);    // large temperatures
LV_FONT_DECLARE(font_text_24);    // body text, plus a few small icons
LV_FONT_DECLARE(font_text_18);    // small text, status icons
LV_FONT_DECLARE(font_icons_96);   // weather icons, large
LV_FONT_DECLARE(font_icons_44);   // weather icons, small

// Font Awesome glyphs (UTF-8)
#define ICON_SUN        "\xEF\x86\x85"
#define ICON_MOON       "\xEF\x86\x86"
#define ICON_CLOUD      "\xEF\x83\x82"
#define ICON_CLOUD_SUN  "\xEF\x9B\x84"
#define ICON_CLOUD_MOON "\xEF\x9B\x83"
#define ICON_RAIN       "\xEF\x9C\xBD"
#define ICON_HEAVY      "\xEF\x9D\x80"
#define ICON_BOLT       "\xEF\x83\xA7"
#define ICON_SNOW       "\xEF\x8B\x9C"
#define ICON_FOG        "\xEF\x9D\x9F"
#define ICON_TINT       "\xEF\x81\x83"
#define ICON_WIND       "\xEF\x9C\xAE"
#define ICON_THERMO     "\xEF\x8B\x89"
#define ICON_HOME       "\xEF\x80\x95"
#define ICON_ARRIVAL    "\xEF\x96\xAF"
#define ICON_PLANE      "\xEF\x81\xB2"
#define DOT             "\xC2\xB7"
#define DEG             "\xC2\xB0"

// ---- Colors ------------------------------------------------------------------
#define COL_BG      lv_color_hex(0x0B0F14)
#define COL_TEXT    lv_color_hex(0xF2F4F7)
#define COL_MUTED   lv_color_hex(0x8A94A6)
#define COL_FAINT   lv_color_hex(0x2A3240)
#define COL_SUN     lv_color_hex(0xFFB547)
#define COL_MOON    lv_color_hex(0xC9D3FF)
#define COL_CLOUD   lv_color_hex(0xB8C2D1)
#define COL_RAIN    lv_color_hex(0x5AC8FA)
#define COL_STORM   lv_color_hex(0xFFD60A)
#define COL_ACCENT  lv_color_hex(0x5AC8FA)

#define SCREEN_W LCD_WIDTH
#define SCREEN_H LCD_HEIGHT
#define PAGE_COUNT 5

// ---- Weather data --------------------------------------------------------------
struct HourForecast {
  int hour;
  float temp;
  int code;
  bool is_day;
};

struct Weather {
  bool valid = false;
  float temp = 0, feels = 0, hi = 0, lo = 0, wind = 0;
  int humidity = 0, code = 0;
  bool is_day = true;
  HourForecast next[4];
  time_t updated = 0;
};

static Weather weather;

// ---- Trending topics -------------------------------------------------------------
struct Trend {
  char name[48];
  long count;      // posts, or -1 if the site didn't list a count
};

struct Trends {
  int n = 0;
  Trend items[TRENDS_COUNT];
  time_t updated = 0;
};

static Trends trends;
static Arrivals arrivals;

// ---- UI objects ----------------------------------------------------------------
static lv_obj_t *tileview;
static lv_obj_t *dots[PAGE_COUNT];

static lv_obj_t *lbl_status, *lbl_time, *lbl_ampm, *lbl_date, *lbl_indoor;
static lv_obj_t *lbl_c_icon, *lbl_c_temp, *lbl_c_cond, *lbl_c_hilo;

static lv_obj_t *lbl_w_icon, *lbl_w_temp, *lbl_w_cond, *lbl_w_hilo, *lbl_w_detail;
static lv_obj_t *lbl_h_time[4], *lbl_h_icon[4], *lbl_h_temp[4];

static lv_obj_t *lbl_t_rank[TRENDS_COUNT], *lbl_t_name[TRENDS_COUNT], *lbl_t_count[TRENDS_COUNT];
static lv_obj_t *lbl_t_status;

static lv_obj_t *lbl_a_status, *lbl_a_empty;
static lv_obj_t *lbl_a_name[MAX_ARRIVALS], *lbl_a_eta[MAX_ARRIVALS], *lbl_a_info[MAX_ARRIVALS];

static lv_obj_t *lbl_sys;
static lv_obj_t *slider_bright;

// ---- Runtime state -------------------------------------------------------------
static SemaphoreHandle_t lvgl_mutex;
static uint8_t day_brightness = DAY_BRIGHTNESS;
static uint8_t current_brightness = 255;
static volatile uint32_t last_touch_ms = 0;
static uint32_t last_weather_fetch_ms = 0;
static uint32_t last_trends_fetch_ms = 0;
static uint32_t last_flights_fetch_ms = 0;
static bool trends_fetched_once = false;
static bool weather_fetched_once = false;
static bool touch_ok = false;
static bool indoor_ok = false;
static float indoor_c = 0, indoor_rh = 0;
static PowerInfo power = {};

static bool ui_lock()   { return xSemaphoreTake(lvgl_mutex, portMAX_DELAY) == pdTRUE; }
static void ui_unlock() { xSemaphoreGive(lvgl_mutex); }

static float to_display_temp(float c) { return USE_FAHRENHEIT ? c * 9.0f / 5.0f + 32.0f : c; }

// ============================================================================
//  Weather code helpers (WMO codes used by Open-Meteo)
// ============================================================================
static const char *weather_text(int code) {
  switch (code) {
    case 0:  return "Clear";
    case 1:  return "Mostly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Light showers";
    case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Storm and hail";
    default: return "--";
  }
}

static const char *weather_icon(int code, bool day) {
  if (code <= 1) return day ? ICON_SUN : ICON_MOON;
  if (code == 2) return day ? ICON_CLOUD_SUN : ICON_CLOUD_MOON;
  if (code == 3) return ICON_CLOUD;
  if (code == 45 || code == 48) return ICON_FOG;
  if (code == 65 || code == 66 || code == 67 || code == 82) return ICON_HEAVY;
  if ((code >= 51 && code <= 63) || code == 80 || code == 81) return ICON_RAIN;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return ICON_SNOW;
  if (code >= 95) return ICON_BOLT;
  return ICON_CLOUD;
}

static lv_color_t weather_color(int code, bool day) {
  if (code <= 2) return day ? COL_SUN : COL_MOON;
  if (code >= 95) return COL_STORM;
  if (code >= 51) return COL_RAIN;
  return COL_CLOUD;
}

// ============================================================================
//  Small UI helpers
// ============================================================================
static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color,
                            lv_align_t align, lv_coord_t x, lv_coord_t y, const char *text) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, text);
  lv_obj_align(l, align, x, y);
  return l;
}

static lv_obj_t *make_row(lv_obj_t *parent, lv_coord_t y, lv_coord_t gap) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, gap, 0);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
  return row;
}

static void make_divider(lv_obj_t *parent, lv_coord_t y, lv_coord_t w) {
  lv_obj_t *line = lv_obj_create(parent);
  lv_obj_remove_style_all(line);
  lv_obj_set_size(line, w, 1);
  lv_obj_set_style_bg_color(line, COL_FAINT, 0);
  lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
  lv_obj_align(line, LV_ALIGN_TOP_MID, 0, y);
}

static lv_obj_t *make_tile(uint8_t col) {
  lv_obj_t *t = lv_tileview_add_tile(tileview, col, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
  lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
  return t;
}

static void update_dots(uint8_t active) {
  for (int i = 0; i < PAGE_COUNT; i++) {
    lv_obj_set_style_bg_color(dots[i], i == active ? COL_TEXT : COL_FAINT, 0);
    lv_obj_set_width(dots[i], i == active ? 20 : 8);
  }
}

static void tileview_changed_cb(lv_event_t *e) {
  lv_obj_t *tile = lv_tileview_get_tile_act(tileview);
  update_dots(lv_obj_get_x(tile) / SCREEN_W);
}

// Backlight is driven by the CH32V006 expander over I2C; only call on the LVGL task.
static void set_brightness(uint8_t pct) {
  if (pct != current_brightness) {
    Board_SetBacklight(pct);
    current_brightness = pct;
  }
}

static void slider_cb(lv_event_t *e) {
  day_brightness = (uint8_t)lv_slider_get_value(slider_bright);
  set_brightness(day_brightness);
}

// ============================================================================
//  Build the three pages
// ============================================================================
static void build_clock_page(lv_obj_t *p) {
  lbl_status = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, 0, 20, "");

  lbl_time = make_label(p, &font_clock_104, COL_TEXT, LV_ALIGN_TOP_MID, USE_24_HOUR ? 0 : -18, 58, "--:--");
  lbl_ampm = make_label(p, &font_text_24, COL_MUTED, LV_ALIGN_TOP_LEFT, 0, 0, "");

  lbl_date   = make_label(p, &font_text_24, COL_MUTED, LV_ALIGN_TOP_MID, 0, 182, "Waiting for time...");
  lbl_indoor = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, 0, 222, "");

  make_divider(p, 266, 220);

  lv_obj_t *row = make_row(p, 288, 16);
  lbl_c_icon = make_label(row, &font_icons_44, COL_CLOUD, LV_ALIGN_DEFAULT, 0, 0, ICON_CLOUD);
  lbl_c_temp = make_label(row, &font_text_56, COL_TEXT, LV_ALIGN_DEFAULT, 0, 0, "--");

  lbl_c_cond = make_label(p, &font_text_24, COL_TEXT,  LV_ALIGN_TOP_MID, 0, 366, "Connecting...");
  lbl_c_hilo = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, 0, 400, "");
}

static void build_weather_page(lv_obj_t *p) {
  make_label(p, &font_text_24, COL_MUTED, LV_ALIGN_TOP_MID, 0, 22, LOCATION_NAME);

  lv_obj_t *row = make_row(p, 62, 18);
  lbl_w_icon = make_label(row, &font_icons_96, COL_CLOUD, LV_ALIGN_DEFAULT, 0, 0, ICON_CLOUD);
  lbl_w_temp = make_label(row, &font_text_56, COL_TEXT, LV_ALIGN_DEFAULT, 0, 0, "--");

  lbl_w_cond   = make_label(p, &font_text_24, COL_TEXT,  LV_ALIGN_TOP_MID, 0, 170, "--");
  lbl_w_hilo   = make_label(p, &font_text_24, COL_MUTED, LV_ALIGN_TOP_MID, 0, 204, "");
  lbl_w_detail = make_label(p, &font_text_24, COL_MUTED, LV_ALIGN_TOP_MID, 0, 238, "");

  make_divider(p, 284, 260);

  // Next four hours, 80 px columns
  for (int i = 0; i < 4; i++) {
    lv_coord_t x = 40 + i * 80 - SCREEN_W / 2;
    lbl_h_time[i] = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, x, 300, "");
    lbl_h_icon[i] = make_label(p, &font_icons_44, COL_CLOUD, LV_ALIGN_TOP_MID, x, 330, "");
    lbl_h_temp[i] = make_label(p, &font_text_24, COL_TEXT, LV_ALIGN_TOP_MID, x, 386, "");
  }
}

static void build_arrivals_page(lv_obj_t *p) {
  make_label(p, &font_text_24, COL_TEXT, LV_ALIGN_TOP_MID, 0, 22, ICON_ARRIVAL "  " AIRPORT_CODE " Arrivals");
  lbl_a_status = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, 0, 54, AIRPORT_NAME);
  lbl_a_empty  = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, 0, 200, "Looking for flights...");

  for (int i = 0; i < MAX_ARRIVALS; i++) {
    lv_coord_t y = 90 + i * 57;
    lbl_a_name[i] = make_label(p, &font_text_24, COL_TEXT, LV_ALIGN_TOP_LEFT, 20, y, "");
    lv_label_set_long_mode(lbl_a_name[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_a_name[i], 200);
    lbl_a_eta[i]  = make_label(p, &font_text_24, COL_ACCENT, LV_ALIGN_TOP_RIGHT, -20, y, "");
    lbl_a_info[i] = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_LEFT, 20, y + 28, "");
    lv_label_set_long_mode(lbl_a_info[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_a_info[i], 282);
  }
}

static void build_trends_page(lv_obj_t *p) {
  make_label(p, &font_text_24, COL_TEXT, LV_ALIGN_TOP_MID, 0, 22, "Trending on X");
  lbl_t_status = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, 0, 54, TRENDS_LOCATION_NAME);

  for (int i = 0; i < TRENDS_COUNT; i++) {
    lv_coord_t y = 92 + i * 34;
    char num[4];
    snprintf(num, sizeof(num), "%d", i + 1);
    lbl_t_rank[i] = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_RIGHT, -(SCREEN_W - 50), y, num);
    lbl_t_name[i] = make_label(p, &font_text_18, COL_TEXT, LV_ALIGN_TOP_LEFT, 62, y, i == 0 ? "Loading..." : "");
    lv_label_set_long_mode(lbl_t_name[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_t_name[i], 178);
    lbl_t_count[i] = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_RIGHT, -26, y, "");
  }
}

static void build_system_page(lv_obj_t *p) {
  make_label(p, &font_text_24, COL_TEXT, LV_ALIGN_TOP_MID, 0, 22, "System");

  lv_obj_t *keys = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_LEFT, 36, 72,
                              "Wi-Fi\nSignal\nIP\nBattery\nIndoor\nUptime\nWeather");
  lv_obj_set_style_text_line_space(keys, 12, 0);
  lbl_sys = make_label(p, &font_text_18, COL_TEXT, LV_ALIGN_TOP_LEFT, 128, 72, "");
  lv_obj_set_style_text_line_space(lbl_sys, 12, 0);

  make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, 0, 330, "Brightness");
  slider_bright = lv_slider_create(p);
  lv_obj_set_size(slider_bright, 230, 12);
  lv_obj_align(slider_bright, LV_ALIGN_TOP_MID, 0, 372);
  lv_slider_set_range(slider_bright, 5, 100);
  lv_slider_set_value(slider_bright, day_brightness, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(slider_bright, lv_color_hex(0x3A4456), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(slider_bright, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider_bright, COL_ACCENT, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider_bright, COL_TEXT, LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider_bright, 8, LV_PART_KNOB);
  lv_obj_set_ext_click_area(slider_bright, 20);
  lv_obj_add_event_cb(slider_bright, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_clear_flag(slider_bright, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_clear_flag(slider_bright, LV_OBJ_FLAG_SCROLL_CHAIN);
}

static void build_ui() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, COL_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  tileview = lv_tileview_create(scr);
  lv_obj_set_size(tileview, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_opa(tileview, LV_OPA_TRANSP, 0);
  lv_obj_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(tileview, tileview_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

  build_clock_page(make_tile(0));
  build_weather_page(make_tile(1));
  build_arrivals_page(make_tile(2));
  build_trends_page(make_tile(3));
  build_system_page(make_tile(4));

  lv_obj_t *dot_row = lv_obj_create(scr);
  lv_obj_remove_style_all(dot_row);
  lv_obj_set_size(dot_row, LV_SIZE_CONTENT, 8);
  lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(dot_row, 8, 0);
  lv_obj_align(dot_row, LV_ALIGN_BOTTOM_MID, 0, -22);
  lv_obj_clear_flag(dot_row, LV_OBJ_FLAG_CLICKABLE);
  for (int i = 0; i < PAGE_COUNT; i++) {
    dots[i] = lv_obj_create(dot_row);
    lv_obj_remove_style_all(dots[i]);
    lv_obj_set_size(dots[i], 8, 8);
    lv_obj_set_style_radius(dots[i], 4, 0);
    lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
  }
  update_dots(0);
}

// ============================================================================
//  UI updates (called with the UI lock held, or from LVGL timers)
// ============================================================================
static void ui_show_weather() {
  if (!weather.valid) return;
  char buf[96];

  const char *icon = weather_icon(weather.code, weather.is_day);
  lv_color_t col = weather_color(weather.code, weather.is_day);

  snprintf(buf, sizeof(buf), "%.0f" DEG, weather.temp);
  lv_label_set_text(lbl_c_temp, buf);
  lv_label_set_text(lbl_w_temp, buf);

  lv_label_set_text(lbl_c_icon, icon);
  lv_obj_set_style_text_color(lbl_c_icon, col, 0);
  lv_label_set_text(lbl_w_icon, icon);
  lv_obj_set_style_text_color(lbl_w_icon, col, 0);

  lv_label_set_text(lbl_c_cond, weather_text(weather.code));
  lv_label_set_text(lbl_w_cond, weather_text(weather.code));

  snprintf(buf, sizeof(buf), "H %.0f" DEG "   L %.0f" DEG, weather.hi, weather.lo);
  lv_label_set_text(lbl_c_hilo, buf);

  snprintf(buf, sizeof(buf), "H %.0f" DEG "   L %.0f" DEG "   " ICON_THERMO " %.0f" DEG,
           weather.hi, weather.lo, weather.feels);
  lv_label_set_text(lbl_w_hilo, buf);

  snprintf(buf, sizeof(buf), ICON_TINT " %d%%      " ICON_WIND " %.0f %s",
           weather.humidity, weather.wind, USE_FAHRENHEIT ? "mph" : "km/h");
  lv_label_set_text(lbl_w_detail, buf);

  for (int i = 0; i < 4; i++) {
    const HourForecast &h = weather.next[i];
    if (USE_24_HOUR) {
      snprintf(buf, sizeof(buf), "%02d:00", h.hour);
    } else {
      int h12 = h.hour % 12 == 0 ? 12 : h.hour % 12;
      snprintf(buf, sizeof(buf), "%d %s", h12, h.hour < 12 ? "AM" : "PM");
    }
    lv_label_set_text(lbl_h_time[i], buf);
    lv_label_set_text(lbl_h_icon[i], weather_icon(h.code, h.is_day));
    lv_obj_set_style_text_color(lbl_h_icon[i], weather_color(h.code, h.is_day), 0);
    snprintf(buf, sizeof(buf), "%.0f" DEG, h.temp);
    lv_label_set_text(lbl_h_temp[i], buf);
  }
}

static void format_count(char *out, size_t n, long c) {
  if (c < 0)             out[0] = 0;
  else if (c >= 1000000) snprintf(out, n, "%.1fM", c / 1000000.0);
  else if (c >= 10000)   snprintf(out, n, "%ldK", c / 1000);
  else if (c >= 1000)    snprintf(out, n, "%.1fK", c / 1000.0);
  else                   snprintf(out, n, "%ld", c);
}

static void ui_show_trends() {
  char buf[24];
  for (int i = 0; i < TRENDS_COUNT; i++) {
    bool has = i < trends.n;
    snprintf(buf, sizeof(buf), "%d", i + 1);
    lv_label_set_text(lbl_t_rank[i], has ? buf : "");
    lv_label_set_text(lbl_t_name[i], has ? trends.items[i].name : "");
    format_count(buf, sizeof(buf), has ? trends.items[i].count : -1);
    lv_label_set_text(lbl_t_count[i], buf);
  }
}

static void format_clock(char *out, size_t n, int hour, int minute, bool with_ampm);

static void ui_show_arrivals() {
  char buf[96];
  for (int i = 0; i < MAX_ARRIVALS; i++) {
    bool has = i < arrivals.n;
    const Arrival &a = arrivals.items[i];
    lv_label_set_text(lbl_a_name[i], has ? a.display : "");
    if (!has) {
      lv_label_set_text(lbl_a_eta[i], "");
      lv_label_set_text(lbl_a_info[i], "");
      continue;
    }
    if (a.eta) {
      struct tm te;
      localtime_r(&a.eta, &te);
      format_clock(buf, sizeof(buf), te.tm_hour, te.tm_min, false);   // estimated arrival time, e.g. "3:52"
    } else {
      buf[0] = 0;
    }
    lv_label_set_text(lbl_a_eta[i], buf);

    char from[32];
    if (a.origin_city[0])  snprintf(from, sizeof(from), "From %s", a.origin_city);
    else if (a.origin[0])  snprintf(from, sizeof(from), "From %s", a.origin);
    else             snprintf(from, sizeof(from), "%s", a.type[0] ? a.type : "Private");
    char alt[16];
    if (a.alt_ft >= 1000) snprintf(alt, sizeof(alt), "%.1fk ft", a.alt_ft / 1000.0f);
    else                  snprintf(alt, sizeof(alt), "%d ft", a.alt_ft);
    snprintf(buf, sizeof(buf), "%s " DOT " %.0f nm " DOT " %s", from, a.dist_nm, alt);
    lv_label_set_text(lbl_a_info[i], buf);
  }
  lv_label_set_text(lbl_a_empty, arrivals.n ? "" : "No arrivals right now");
}

static void format_uptime(char *out, size_t n) {
  uint32_t s = millis() / 1000;
  uint32_t d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
  if (d) snprintf(out, n, "%lud %luh %lum", (unsigned long)d, (unsigned long)h, (unsigned long)m);
  else   snprintf(out, n, "%luh %lum", (unsigned long)h, (unsigned long)m);
}

static const char *battery_symbol(int soc) {
  if (soc >= 90) return LV_SYMBOL_BATTERY_FULL;
  if (soc >= 65) return LV_SYMBOL_BATTERY_3;
  if (soc >= 35) return LV_SYMBOL_BATTERY_2;
  if (soc >= 10) return LV_SYMBOL_BATTERY_1;
  return LV_SYMBOL_BATTERY_EMPTY;
}

static void format_clock(char *out, size_t n, int hour, int minute, bool with_ampm) {
  if (USE_24_HOUR) {
    snprintf(out, n, "%02d:%02d", hour, minute);
  } else {
    int h12 = hour % 12 == 0 ? 12 : hour % 12;
    if (with_ampm) snprintf(out, n, "%d:%02d %s", h12, minute, hour < 12 ? "AM" : "PM");
    else           snprintf(out, n, "%d:%02d", h12, minute);
  }
}

// Every second on the LVGL task: clock, status line, system page, brightness.
static void clock_timer_cb(lv_timer_t *t) {
  char buf[256];
  time_t now = time(nullptr);
  struct tm tm_now;
  localtime_r(&now, &tm_now);
  bool time_ok = tm_now.tm_year > (2024 - 1900);

  if (time_ok) {
    format_clock(buf, sizeof(buf), tm_now.tm_hour, tm_now.tm_min, false);
    lv_label_set_text(lbl_time, buf);
    if (!USE_24_HOUR) {
      lv_label_set_text(lbl_ampm, tm_now.tm_hour < 12 ? "AM" : "PM");
      lv_obj_align_to(lbl_ampm, lbl_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -16);
    }
    char day[24];
    strftime(day, sizeof(day), "%A, %B", &tm_now);
    snprintf(buf, sizeof(buf), "%s %d", day, tm_now.tm_mday);
    lv_label_set_text(lbl_date, buf);
  }

  // Status line: Wi-Fi + battery/USB
  bool wifi_up = WiFi.status() == WL_CONNECTED;
  const char *wifi_sym = wifi_up ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE;
  if (power.ok && power.battery && power.percent >= 0) {
    snprintf(buf, sizeof(buf), "%s     %s%s  %d%%", wifi_sym,
             power.charging ? LV_SYMBOL_CHARGE " " : "", battery_symbol(power.percent), power.percent);
  } else {
    snprintf(buf, sizeof(buf), "%s     " LV_SYMBOL_USB, wifi_sym);
  }
  lv_label_set_text(lbl_status, buf);

  // Indoor reading on the clock page
  char indoor[48] = "--";
  if (indoor_ok) {
    snprintf(indoor, sizeof(indoor), "%.0f" DEG "  " ICON_TINT " %.0f%%", to_display_temp(indoor_c), indoor_rh);
    snprintf(buf, sizeof(buf), ICON_HOME "  Indoor  %s", indoor);
    lv_label_set_text(lbl_indoor, buf);
  }

  // System page
  char up[24], updated[24] = "never", batt[40], ssid[17] = "not connected";
  format_uptime(up, sizeof(up));
  if (weather.updated) {
    struct tm tu;
    localtime_r(&weather.updated, &tu);
    format_clock(updated, sizeof(updated), tu.tm_hour, tu.tm_min, true);
  }
  if (power.ok && power.battery) {
    snprintf(batt, sizeof(batt), "%d%%  %.2f V%s", power.percent, power.mv / 1000.0f,
             power.charging ? "  charging" : "");
  } else {
    snprintf(batt, sizeof(batt), "USB power");
  }
  if (wifi_up) {
    strncpy(ssid, WiFi.SSID().c_str(), sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = 0;
  }
  snprintf(buf, sizeof(buf), "%s\n%s\n%s\n%s\n%s\n%s\n%s",
           ssid,
           wifi_up ? (String(WiFi.RSSI()) + " dBm").c_str() : "--",
           wifi_up ? WiFi.localIP().toString().c_str() : "--",
           batt, indoor, up, updated);
  lv_label_set_text(lbl_sys, buf);

  // Arrivals page subtitle
  if (arrivals.updated) {
    struct tm ta;
    localtime_r(&arrivals.updated, &ta);
    char at[16];
    format_clock(at, sizeof(at), ta.tm_hour, ta.tm_min, true);
    snprintf(buf, sizeof(buf), "%d inbound  " DOT "  updated %s", arrivals.n, at);
    lv_label_set_text(lbl_a_status, buf);
  }

  // Trends page subtitle: location and when the list was fetched
  if (trends.updated) {
    struct tm tt;
    localtime_r(&trends.updated, &tt);
    char at[16];
    format_clock(at, sizeof(at), tt.tm_hour, tt.tm_min, true);
    snprintf(buf, sizeof(buf), "%s  " DOT "  %s", TRENDS_LOCATION_NAME, at);
    lv_label_set_text(lbl_t_status, buf);
  }

  // Brightness: dim at night, a tap wakes it for a while
  bool night = time_ok && (NIGHT_START_HOUR > NIGHT_END_HOUR
                             ? (tm_now.tm_hour >= NIGHT_START_HOUR || tm_now.tm_hour < NIGHT_END_HOUR)
                             : (tm_now.tm_hour >= NIGHT_START_HOUR && tm_now.tm_hour < NIGHT_END_HOUR));
  bool recently_touched = last_touch_ms && (millis() - last_touch_ms < WAKE_SECONDS * 1000UL);
  set_brightness(night && !recently_touched ? NIGHT_BRIGHTNESS : day_brightness);
}

// Every 15 s on the LVGL task, so all I2C traffic stays on one thread.
static void sensor_timer_cb(lv_timer_t *t) {
  pmu_read(&power);
  float c, rh;
  if (shtc3_read(&c, &rh)) {
    indoor_c = c;
    indoor_rh = rh;
    indoor_ok = true;
  }
}

// ============================================================================
//  LVGL glue: display flush, touch input, and the LVGL task
// ============================================================================
static void display_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  size_t bytes = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1) * sizeof(lv_color_t);
  if (!lcd_send_area(area->x1, area->y1, area->x2, area->y2, color_p, bytes)) {
    Serial.println("[lcd] transfer failed");
  }
  lv_disp_flush_ready(drv);
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  static uint16_t last_x = SCREEN_W / 2, last_y = SCREEN_H / 2;
  uint16_t x[2] = {}, y[2] = {};
  uint8_t count = 0;
  if (touch_ok && FT6336_Read(x, y, &count, 1) && count) {
    last_x = x[0];
    last_y = y[0];
    data->state = LV_INDEV_STATE_PR;
    last_touch_ms = millis();
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
  data->point.x = last_x;
  data->point.y = last_y;
}

static void lvgl_task(void *arg) {
  uint32_t last = millis();
  while (true) {
    uint32_t now = millis();
    uint32_t wait = 5;
    if (ui_lock()) {
      lv_tick_inc(now - last);
      wait = lv_timer_handler();
      ui_unlock();
    }
    last = now;
    if (wait < 2) wait = 2;
    if (wait > 20) wait = 20;
    vTaskDelay(pdMS_TO_TICKS(wait));
  }
}

// ============================================================================
//  Networking (runs in loop(), outside the LVGL task)
// ============================================================================
// Lists every network the radio can see, so a missing or misnamed network is obvious in the log.
static void wifi_scan_report() {
  int n = WiFi.scanNetworks();
  Serial.printf("[wifi] scan found %d networks:\n", n);
  for (int i = 0; i < n && i < 20; i++) {
    Serial.printf("[wifi]   \"%s\"  ch %d  %d dBm  auth %d%s\n", WiFi.SSID(i).c_str(), (int)WiFi.channel(i),
                  (int)WiFi.RSSI(i), (int)WiFi.encryptionType(i), WiFi.SSID(i) == WIFI_SSID ? "   <-- yours" : "");
  }
  WiFi.scanDelete();
}

// Copies trend text into out, decoding the common HTML entities and dropping characters
// the display fonts can't draw (anything outside printable ASCII).
static void clean_trend_name(const String &in, char *out, size_t n) {
  static const struct { const char *ent; char ch; } ents[] = {
    {"&amp;", '&'}, {"&#39;", '\''}, {"&#039;", '\''}, {"&quot;", '"'}, {"&lt;", '<'}, {"&gt;", '>'},
  };
  size_t o = 0;
  for (size_t i = 0; i < in.length() && o + 1 < n; i++) {
    char c = in[i];
    if (c == '&') {
      bool matched = false;
      for (auto &e : ents) {
        size_t len = strlen(e.ent);
        if (in.substring(i, i + len) == e.ent) { out[o++] = e.ch; i += len - 1; matched = true; break; }
      }
      if (matched) continue;
    }
    if ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7F) out[o++] = c;
  }
  while (o > 0 && out[o - 1] == ' ') o--;   // trim trailing spaces left by dropped emoji
  out[o] = 0;
}

// Reads the most recent hour of trends from trends24.in. The page is ~250 KB, so it is
// parsed as a stream: find the first <ol class=trend-card__list>, then read each item.
static bool fetch_trends() {
  String url = String("https://trends24.in/") + TRENDS_LOCATION + "/";

  WiFiClientSecure client;
  client.setInsecure();      // public page, nothing sent; skips certificate checks
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("Mozilla/5.0 (DeskDash; ESP32-C5)");
  if (!http.begin(client, url)) return false;
  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("[trends] HTTP %d (%s), free heap %u\n", status, http.errorToString(status).c_str(),
                  (unsigned)ESP.getFreeHeap());
    http.end();
    return false;
  }

  Stream &s = http.getStream();
  s.setTimeout(5000);
  Trends t;
  if (s.find("<ol class=trend-card__list>")) {
    while (t.n < TRENDS_COUNT && s.findUntil((char *)"class=trend-link>", (char *)"</ol>")) {
      String name = s.readStringUntil('<');
      long count = -1;
      if (s.findUntil((char *)"data-count=\"", (char *)"</li>")) {
        String c = s.readStringUntil('"');
        if (c.length()) count = c.toInt();
      }
      clean_trend_name(name, t.items[t.n].name, sizeof(t.items[t.n].name));
      if (t.items[t.n].name[0]) {          // skip trends written entirely in other scripts
        t.items[t.n].count = count;
        t.n++;
      }
    }
  }
  http.end();

  if (t.n == 0) {
    Serial.println("[trends] no trends found (page layout may have changed)");
    return false;
  }
  t.updated = time(nullptr);
  if (ui_lock()) {
    trends = t;
    ui_show_trends();
    ui_unlock();
  }
  Serial.printf("[trends] %d trends, #1: %s\n", t.n, t.items[0].name);
  return true;
}

static bool fetch_weather() {
  char url[512];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,wind_speed_10m"
           "&hourly=temperature_2m,weather_code,is_day"
           "&daily=temperature_2m_max,temperature_2m_min"
           "&temperature_unit=%s&wind_speed_unit=%s&timezone=auto&forecast_days=2",
           (double)LATITUDE, (double)LONGITUDE,
           USE_FAHRENHEIT ? "fahrenheit" : "celsius", USE_FAHRENHEIT ? "mph" : "kmh");

  // Plain HTTP: the data is public and read-only, and skipping TLS saves ~40 KB of internal RAM.
  WiFiClient client;
  HTTPClient http;
  http.useHTTP10(true);      // no chunked encoding, so the JSON parser can read the stream directly
  http.setTimeout(10000);
  if (!http.begin(client, url)) return false;
  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("[weather] HTTP %d (%s), free heap %u\n", status, http.errorToString(status).c_str(),
                  (unsigned)ESP.getFreeHeap());
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("[weather] JSON error: %s\n", err.c_str());
    return false;
  }

  Weather w;
  JsonObject cur = doc["current"];
  w.temp     = cur["temperature_2m"] | 0.0f;
  w.feels    = cur["apparent_temperature"] | 0.0f;
  w.humidity = cur["relative_humidity_2m"] | 0;
  w.code     = cur["weather_code"] | 0;
  w.is_day   = (cur["is_day"] | 1) == 1;
  w.wind     = cur["wind_speed_10m"] | 0.0f;
  w.hi       = doc["daily"]["temperature_2m_max"][0] | 0.0f;
  w.lo       = doc["daily"]["temperature_2m_min"][0] | 0.0f;

  // "2026-10-21T12:30" -> hour 12. Hourly data starts at 00:00 today, so index == hour.
  const char *ct = cur["time"] | "";
  int cur_hour = strlen(ct) >= 13 ? atoi(ct + 11) : 0;
  JsonArray ht = doc["hourly"]["temperature_2m"];
  JsonArray hc = doc["hourly"]["weather_code"];
  JsonArray hd = doc["hourly"]["is_day"];
  for (int i = 0; i < 4; i++) {
    int idx = cur_hour + 1 + i;
    w.next[i].hour   = idx % 24;
    w.next[i].temp   = ht[idx] | 0.0f;
    w.next[i].code   = hc[idx] | 0;
    w.next[i].is_day = (hd[idx] | 1) == 1;
  }
  w.updated = time(nullptr);
  w.valid = true;

  if (ui_lock()) {
    weather = w;
    ui_show_weather();
    ui_unlock();
  }
  Serial.printf("[weather] %.1f, code %d, %s\n", w.temp, w.code, weather_text(w.code));
  return true;
}

// ============================================================================
//  Arduino entry points
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nDeskDash (ESP32-C5-Touch-LCD-3.5) starting");

  // Board power, resets and backlight live on the CH32V006 I/O expander
  if (!Board_IO_Init(Wire)) Serial.println("[board] CH32V006 I/O expander not found");
  Board_SetBacklight(0);
  touch_ok = FT6336_Init();   // resets touch; must happen before the LCD init
  Board_LCD_Reset();
  if (!lcd_begin()) Serial.println("[lcd] init failed");

  pmu_begin();
  pmu_read(&power);
  indoor_ok = shtc3_begin() && shtc3_read(&indoor_c, &indoor_rh);
  Serial.printf("[sensors] indoor sensor %s\n", indoor_ok ? "ok" : "not found");

  // LVGL draw buffers in internal DMA-capable RAM, allocated before Wi-Fi starts
  const size_t buf_px = SCREEN_W * 40;
  lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!buf1 || !buf2) Serial.println("[lvgl] draw buffer allocation failed");

  lv_init();
  static lv_disp_draw_buf_t draw_buf;
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_px);
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_W;
  disp_drv.ver_res = SCREEN_H;
  disp_drv.flush_cb = display_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read_cb;
  lv_indev_drv_register(&indev_drv);

  build_ui();
  lv_timer_create(clock_timer_cb, 1000, NULL);
  lv_timer_create(sensor_timer_cb, 15000, NULL);
  lv_refr_now(NULL);
  set_brightness(day_brightness);

  lvgl_mutex = xSemaphoreCreateMutex();
  xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 2, NULL);

  // Wi-Fi and time
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
      Serial.printf("[wifi] disconnected, reason %d\n", info.wifi_sta_disconnected.reason);
    else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
      Serial.printf("[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());
  });
  WiFi.mode(WIFI_STA);
  WiFi.setBandMode(WIFI_BAND_MODE_AUTO);   // allow both 2.4 GHz and 5 GHz
  wifi_scan_report();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTzTime(TIMEZONE, "pool.ntp.org", "time.nist.gov");
}

void loop() {
  static uint32_t wifi_lost_since = 0;

  if (WiFi.status() == WL_CONNECTED) {
    wifi_lost_since = 0;
    uint32_t interval = (weather_fetched_once ? WEATHER_REFRESH_MINUTES * 60UL : 30UL) * 1000UL;
    if (last_weather_fetch_ms == 0 || millis() - last_weather_fetch_ms > interval) {
      last_weather_fetch_ms = millis();
      if (fetch_weather()) weather_fetched_once = true;
    }
    time_t now_t = time(nullptr);
    struct tm now_tm;
    localtime_r(&now_t, &now_tm);
    bool night = NIGHT_START_HOUR > NIGHT_END_HOUR
                   ? (now_tm.tm_hour >= NIGHT_START_HOUR || now_tm.tm_hour < NIGHT_END_HOUR)
                   : (now_tm.tm_hour >= NIGHT_START_HOUR && now_tm.tm_hour < NIGHT_END_HOUR);
    uint32_t f_interval = (night ? ARRIVALS_NIGHT_REFRESH_SECONDS : ARRIVALS_REFRESH_SECONDS) * 1000UL;
    if (last_flights_fetch_ms == 0 || millis() - last_flights_fetch_ms > f_interval) {
      last_flights_fetch_ms = millis();
      Arrivals a;
      if (flights_fetch(AIRPORT_ICAO, AIRPORT_LAT, AIRPORT_LON, ARRIVALS_RADIUS_NM, &a) && ui_lock()) {
        arrivals = a;
        ui_show_arrivals();
        ui_unlock();
      }
    }
    uint32_t t_interval = (trends_fetched_once ? TRENDS_REFRESH_MINUTES * 60UL : 60UL) * 1000UL;
    if (last_trends_fetch_ms == 0 || millis() - last_trends_fetch_ms > t_interval) {
      last_trends_fetch_ms = millis();
      if (fetch_trends()) trends_fetched_once = true;
    }
  } else {
    if (wifi_lost_since == 0) wifi_lost_since = millis();
    if (millis() - wifi_lost_since > 30000) {   // retry every 30 s
      Serial.printf("[wifi] not connected (status %d), retrying\n", (int)WiFi.status());
      WiFi.disconnect();
      delay(200);
      wifi_scan_report();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      wifi_lost_since = millis();
    }
  }
  delay(200);
}
