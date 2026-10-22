// ============================================================================
//  DeskDash core API — everything a page plugin needs.
//
//  A plugin is one folder under src/plugins/<name>/ that defines a DeskDashPlugin:
//
//    extern const DeskDashPlugin plugin_hello = {   // "extern" makes it visible to DeskDash.ino
//      "Hello",          // name (for logs)
//      hello_build,      // build(page): create LVGL objects on the page (UI lock held)
//      hello_tick,       // tick(): every second on the UI thread (may be nullptr)
//      hello_poll,       // poll(): repeatedly from loop() for network work (may be nullptr)
//    };
//
//  Then add &plugin_hello to the PAGES list in DeskDash.ino. Rules:
//   - build() and tick() run on the LVGL thread: change UI freely.
//   - poll() runs on the Arduino loop thread: do HTTP there, keep your own timing,
//     and wrap any UI change in ui_lock() / ui_unlock().
//   - Only touch I2C from build()/tick() (the LVGL thread owns the bus).
// ============================================================================
#pragma once
#include <Arduino.h>
#include <time.h>
#include "../lvgl/lvgl.h"
#include "../../config.h"

// ---- Defaults for settings added after the first release ------------------------
#ifndef AIRPORT_CODE
#define AIRPORT_CODE            "PBI"   // default airport; change it on screen by tapping the page title
#endif
#ifndef ARRIVALS_RADIUS_NM
#define ARRIVALS_RADIUS_NM      60
#endif
#ifndef ARRIVALS_REFRESH_SECONDS
#define ARRIVALS_REFRESH_SECONDS 60     // positions come from adsb.fi (1 request/second allowed)
#endif
#ifndef ARRIVALS_NIGHT_REFRESH_SECONDS
#define ARRIVALS_NIGHT_REFRESH_SECONDS ARRIVALS_REFRESH_SECONDS
#endif
#ifndef AUTO_CYCLE_SECONDS
#define AUTO_CYCLE_SECONDS      60      // show the next page this often to avoid burn-in (0 = off)
#endif
#ifndef LANDED_KEEP_MINUTES
#define LANDED_KEEP_MINUTES     30      // how long landed flights stay on the Arrivals page
#endif

// ---- Plugin interface ------------------------------------------------------------
struct DeskDashPlugin {
  const char *name;
  void (*build)(lv_obj_t *page);
  void (*tick)();
  void (*poll)();
};

// ---- Screen ------------------------------------------------------------------------
#define SCREEN_W 320
#define SCREEN_H 480

// ---- Fonts (src/fonts) -------------------------------------------------------------
LV_FONT_DECLARE(font_clock_104);  // digits, colon and dash for the big clock
LV_FONT_DECLARE(font_text_56);    // large temperatures (digits, minus, degree)
LV_FONT_DECLARE(font_text_24);    // body text plus a few icons
LV_FONT_DECLARE(font_text_18);    // small text plus status icons
LV_FONT_DECLARE(font_icons_96);   // weather icons, large
LV_FONT_DECLARE(font_icons_44);   // weather icons, small

// Font Awesome glyphs (UTF-8). Check which font contains a glyph before using it.
#define ICON_SUN        "\xEF\x86\x85"   // icons_96, icons_44
#define ICON_MOON       "\xEF\x86\x86"
#define ICON_CLOUD      "\xEF\x83\x82"
#define ICON_CLOUD_SUN  "\xEF\x9B\x84"
#define ICON_CLOUD_MOON "\xEF\x9B\x83"
#define ICON_RAIN       "\xEF\x9C\xBD"
#define ICON_HEAVY      "\xEF\x9D\x80"
#define ICON_BOLT       "\xEF\x83\xA7"
#define ICON_SNOW       "\xEF\x8B\x9C"
#define ICON_FOG        "\xEF\x9D\x9F"
#define ICON_TINT       "\xEF\x81\x83"   // text_24, text_18
#define ICON_WIND       "\xEF\x9C\xAE"   // text_24
#define ICON_THERMO     "\xEF\x8B\x89"   // text_24
#define ICON_HOME       "\xEF\x80\x95"   // text_24, text_18
#define ICON_ARRIVAL    "\xEF\x96\xAF"   // text_24
#define ICON_DEPARTURE  "\xEF\x96\xB0"   // text_24
#define ICON_CARET_DOWN "\xEF\x83\x97"   // text_24
#define ICON_PLANE      "\xEF\x81\xB2"   // text_18
#define DOT             "\xC2\xB7"
#define DEG             "\xC2\xB0"

// ---- Colors ------------------------------------------------------------------------
#define COL_BG      lv_color_hex(0x0B0F14)
#define COL_TEXT    lv_color_hex(0xF2F4F7)
#define COL_MUTED   lv_color_hex(0x8A94A6)
#define COL_FAINT   lv_color_hex(0x2A3240)
#define COL_PANEL   lv_color_hex(0x161C24)
#define COL_SUN     lv_color_hex(0xFFB547)
#define COL_MOON    lv_color_hex(0xC9D3FF)
#define COL_CLOUD   lv_color_hex(0xB8C2D1)
#define COL_RAIN    lv_color_hex(0x5AC8FA)
#define COL_STORM   lv_color_hex(0xFFD60A)
#define COL_ACCENT  lv_color_hex(0x5AC8FA)
#define COL_GOOD    lv_color_hex(0x5EE08A)

// ---- UI helpers (UI thread only) ---------------------------------------------------
lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color,
                     lv_align_t align, lv_coord_t x, lv_coord_t y, const char *text);
lv_obj_t *make_row(lv_obj_t *parent, lv_coord_t y, lv_coord_t gap);
void make_divider(lv_obj_t *parent, lv_coord_t y, lv_coord_t w);

// Lock before touching LVGL from poll() / the loop thread.
bool ui_lock();
void ui_unlock();

// ---- Time --------------------------------------------------------------------------
bool time_is_valid();
bool is_night();                                       // inside NIGHT_START_HOUR..NIGHT_END_HOUR
void format_clock(char *out, size_t n, int hour, int minute, bool with_ampm);
void format_time(char *out, size_t n, time_t t, bool with_ampm);   // local clock time of t
float to_display_temp(float celsius);

// ---- Board state (filled by the core) ------------------------------------------------
struct PowerState {
  bool ok, battery, charging, usb;
  int percent;
  uint16_t mv;
};
const PowerState &dd_power();
bool dd_indoor(float *temp_c, float *humidity);       // false until the sensor has a reading
bool dd_wifi_up();

uint8_t dd_day_brightness();
void dd_set_day_brightness(uint8_t pct, bool save);   // UI thread only

// ---- Settings saved on the board (flash, survives power-off) -------------------------
String dd_setting_get(const char *key, const char *fallback);
void dd_setting_set(const char *key, const char *value);
