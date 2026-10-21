// DeskDash core: shared helpers for plugins. Board state (power, indoor sensor,
// brightness) is owned by DeskDash.ino, which fills it through core_internal.h.
#include "deskdash.h"
#include "core_internal.h"
#include <WiFi.h>
#include <Preferences.h>

// ---- UI helpers --------------------------------------------------------------------
lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color,
                     lv_align_t align, lv_coord_t x, lv_coord_t y, const char *text) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, text);
  lv_obj_align(l, align, x, y);
  return l;
}

lv_obj_t *make_row(lv_obj_t *parent, lv_coord_t y, lv_coord_t gap) {
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

void make_divider(lv_obj_t *parent, lv_coord_t y, lv_coord_t w) {
  lv_obj_t *line = lv_obj_create(parent);
  lv_obj_remove_style_all(line);
  lv_obj_set_size(line, w, 1);
  lv_obj_set_style_bg_color(line, COL_FAINT, 0);
  lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
  lv_obj_align(line, LV_ALIGN_TOP_MID, 0, y);
}

// ---- Locking -----------------------------------------------------------------------
static SemaphoreHandle_t ui_mutex = nullptr;

void core_init_lock() { ui_mutex = xSemaphoreCreateRecursiveMutex(); }

bool ui_lock() { return ui_mutex && xSemaphoreTakeRecursive(ui_mutex, portMAX_DELAY) == pdTRUE; }
void ui_unlock() { if (ui_mutex) xSemaphoreGiveRecursive(ui_mutex); }

// ---- Time --------------------------------------------------------------------------
bool time_is_valid() {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  return t.tm_year > (2024 - 1900);
}

bool is_night() {
  if (!time_is_valid()) return false;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  return NIGHT_START_HOUR > NIGHT_END_HOUR
           ? (t.tm_hour >= NIGHT_START_HOUR || t.tm_hour < NIGHT_END_HOUR)
           : (t.tm_hour >= NIGHT_START_HOUR && t.tm_hour < NIGHT_END_HOUR);
}

void format_clock(char *out, size_t n, int hour, int minute, bool with_ampm) {
  if (USE_24_HOUR) {
    snprintf(out, n, "%02d:%02d", hour, minute);
  } else {
    int h12 = hour % 12 == 0 ? 12 : hour % 12;
    if (with_ampm) snprintf(out, n, "%d:%02d %s", h12, minute, hour < 12 ? "AM" : "PM");
    else           snprintf(out, n, "%d:%02d", h12, minute);
  }
}

void format_time(char *out, size_t n, time_t t, bool with_ampm) {
  struct tm tt;
  localtime_r(&t, &tt);
  format_clock(out, n, tt.tm_hour, tt.tm_min, with_ampm);
}

float to_display_temp(float c) { return USE_FAHRENHEIT ? c * 9.0f / 5.0f + 32.0f : c; }

// ---- Board state ---------------------------------------------------------------------
static PowerState power_state = {};
static bool indoor_valid = false;
static float indoor_temp = 0, indoor_rh = 0;
static uint8_t day_brightness = DAY_BRIGHTNESS;

const PowerState &dd_power() { return power_state; }
void core_set_power(const PowerState &p) { power_state = p; }

bool dd_indoor(float *t, float *h) {
  if (!indoor_valid) return false;
  *t = indoor_temp;
  *h = indoor_rh;
  return true;
}
void core_set_indoor(float t, float h) {
  indoor_temp = t;
  indoor_rh = h;
  indoor_valid = true;
}

bool dd_wifi_up() { return WiFi.status() == WL_CONNECTED; }

uint8_t dd_day_brightness() { return day_brightness; }
void dd_set_day_brightness(uint8_t pct, bool save) {
  day_brightness = pct;
  if (save) dd_setting_set("brightness", String(pct).c_str());
}
void core_load_brightness() {
  day_brightness = (uint8_t)dd_setting_get("brightness", String(DAY_BRIGHTNESS).c_str()).toInt();
  if (day_brightness < 5) day_brightness = DAY_BRIGHTNESS;
}

// ---- Settings (NVS) ----------------------------------------------------------------------
String dd_setting_get(const char *key, const char *fallback) {
  Preferences p;
  if (!p.begin("deskdash", true)) return String(fallback);
  String v = p.getString(key, fallback);
  p.end();
  return v;
}

void dd_setting_set(const char *key, const char *value) {
  Preferences p;
  if (!p.begin("deskdash", false)) return;
  p.putString(key, value);
  p.end();
}
