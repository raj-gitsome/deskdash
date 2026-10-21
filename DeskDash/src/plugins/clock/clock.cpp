// Clock page: time, date, Wi-Fi/battery status, indoor reading and current weather.
// The weather row reads from the weather plugin (that plugin fetches the data).
#include "../../core/deskdash.h"
#include "../weather/weather.h"
#include <WiFi.h>

static lv_obj_t *lbl_status, *lbl_time, *lbl_ampm, *lbl_date, *lbl_indoor;
static lv_obj_t *lbl_icon, *lbl_temp, *lbl_cond, *lbl_hilo;
static uint32_t shown_weather = 0;

static void build(lv_obj_t *p) {
  lbl_status = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, 0, 20, "");

  lbl_time = make_label(p, &font_clock_104, COL_TEXT, LV_ALIGN_TOP_MID, USE_24_HOUR ? 0 : -18, 58, "--:--");
  lbl_ampm = make_label(p, &font_text_24, COL_MUTED, LV_ALIGN_TOP_LEFT, 0, 0, "");

  lbl_date   = make_label(p, &font_text_24, COL_MUTED, LV_ALIGN_TOP_MID, 0, 182, "Waiting for time...");
  lbl_indoor = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, 0, 222, "");

  make_divider(p, 266, 220);

  lv_obj_t *row = make_row(p, 288, 16);
  lbl_icon = make_label(row, &font_icons_44, COL_CLOUD, LV_ALIGN_DEFAULT, 0, 0, ICON_CLOUD);
  lbl_temp = make_label(row, &font_text_56, COL_TEXT, LV_ALIGN_DEFAULT, 0, 0, "--");

  lbl_cond = make_label(p, &font_text_24, COL_TEXT,  LV_ALIGN_TOP_MID, 0, 366, "Connecting...");
  lbl_hilo = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, 0, 400, "");
}

static const char *battery_symbol(int soc) {
  if (soc >= 90) return LV_SYMBOL_BATTERY_FULL;
  if (soc >= 65) return LV_SYMBOL_BATTERY_3;
  if (soc >= 35) return LV_SYMBOL_BATTERY_2;
  if (soc >= 10) return LV_SYMBOL_BATTERY_1;
  return LV_SYMBOL_BATTERY_EMPTY;
}

static void tick() {
  char buf[96];
  if (time_is_valid()) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    format_clock(buf, sizeof(buf), t.tm_hour, t.tm_min, false);
    lv_label_set_text(lbl_time, buf);
    if (!USE_24_HOUR) {
      lv_label_set_text(lbl_ampm, t.tm_hour < 12 ? "AM" : "PM");
      lv_obj_align_to(lbl_ampm, lbl_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -16);
    }
    char day[24];
    strftime(day, sizeof(day), "%A, %B", &t);
    snprintf(buf, sizeof(buf), "%s %d", day, t.tm_mday);
    lv_label_set_text(lbl_date, buf);
  }

  // Status line: Wi-Fi + battery/USB
  const PowerState &pw = dd_power();
  const char *wifi_sym = dd_wifi_up() ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE;
  if (pw.ok && pw.battery && pw.percent >= 0)
    snprintf(buf, sizeof(buf), "%s     %s%s  %d%%", wifi_sym, pw.charging ? LV_SYMBOL_CHARGE " " : "",
             battery_symbol(pw.percent), pw.percent);
  else
    snprintf(buf, sizeof(buf), "%s     " LV_SYMBOL_USB, wifi_sym);
  lv_label_set_text(lbl_status, buf);

  float tc, rh;
  if (dd_indoor(&tc, &rh)) {
    snprintf(buf, sizeof(buf), ICON_HOME "  Indoor  %.0f" DEG "  " ICON_TINT " %.0f%%", to_display_temp(tc), rh);
    lv_label_set_text(lbl_indoor, buf);
  }

  // Weather row: refresh only when the weather plugin has new data
  if (weather_version() != shown_weather) {
    shown_weather = weather_version();
    const Weather &w = weather_data();
    snprintf(buf, sizeof(buf), "%.0f" DEG, w.temp);
    lv_label_set_text(lbl_temp, buf);
    lv_label_set_text(lbl_icon, weather_icon(w.code, w.is_day));
    lv_obj_set_style_text_color(lbl_icon, weather_color(w.code, w.is_day), 0);
    lv_label_set_text(lbl_cond, weather_text(w.code));
    snprintf(buf, sizeof(buf), "H %.0f" DEG "   L %.0f" DEG, w.hi, w.lo);
    lv_label_set_text(lbl_hilo, buf);
  }
}

extern const DeskDashPlugin plugin_clock = {"Clock", build, tick, nullptr};
