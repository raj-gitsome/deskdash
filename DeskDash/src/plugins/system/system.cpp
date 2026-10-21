// System page: Wi-Fi, battery, indoor reading, uptime, last weather update, brightness.
#include "../../core/deskdash.h"
#include "../weather/weather.h"
#include <WiFi.h>

static lv_obj_t *lbl_values;
static lv_obj_t *slider;

static void slider_changed(lv_event_t *e) {
  // Save to flash only when the finger lifts, not on every step of the drag
  bool released = lv_event_get_code(e) == LV_EVENT_RELEASED;
  dd_set_day_brightness((uint8_t)lv_slider_get_value(slider), released);
}

static void build(lv_obj_t *p) {
  make_label(p, &font_text_24, COL_TEXT, LV_ALIGN_TOP_MID, 0, 22, "System");

  lv_obj_t *keys = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_LEFT, 36, 72,
                              "Wi-Fi\nSignal\nIP\nBattery\nIndoor\nUptime\nWeather");
  lv_obj_set_style_text_line_space(keys, 12, 0);
  lbl_values = make_label(p, &font_text_18, COL_TEXT, LV_ALIGN_TOP_LEFT, 128, 72, "");
  lv_obj_set_style_text_line_space(lbl_values, 12, 0);

  make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, 0, 330, "Brightness");
  slider = lv_slider_create(p);
  lv_obj_set_size(slider, 230, 12);
  lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 372);
  lv_slider_set_range(slider, 5, 100);
  lv_slider_set_value(slider, dd_day_brightness(), LV_ANIM_OFF);
  lv_obj_set_style_bg_color(slider, lv_color_hex(0x3A4456), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, COL_ACCENT, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, COL_TEXT, LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
  lv_obj_set_ext_click_area(slider, 20);
  lv_obj_add_event_cb(slider, slider_changed, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(slider, slider_changed, LV_EVENT_RELEASED, NULL);
  lv_obj_clear_flag(slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_clear_flag(slider, LV_OBJ_FLAG_SCROLL_CHAIN);   // dragging it doesn't swipe the page
}

static void tick() {
  char buf[256], up[24], updated[24] = "never", batt[40], indoor[32] = "--", ssid[17] = "not connected";
  uint32_t s = millis() / 1000;
  uint32_t d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
  if (d) snprintf(up, sizeof(up), "%lud %luh %lum", (unsigned long)d, (unsigned long)h, (unsigned long)m);
  else   snprintf(up, sizeof(up), "%luh %lum", (unsigned long)h, (unsigned long)m);

  if (weather_data().updated) format_time(updated, sizeof(updated), weather_data().updated, true);

  const PowerState &pw = dd_power();
  if (pw.ok && pw.battery)
    snprintf(batt, sizeof(batt), "%d%%  %.2f V%s", pw.percent, pw.mv / 1000.0f, pw.charging ? "  charging" : "");
  else
    snprintf(batt, sizeof(batt), "USB power");

  float tc, rh;
  if (dd_indoor(&tc, &rh)) snprintf(indoor, sizeof(indoor), "%.0f" DEG "  %.0f%%", to_display_temp(tc), rh);

  bool up_wifi = dd_wifi_up();
  if (up_wifi) {
    strncpy(ssid, WiFi.SSID().c_str(), sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = 0;
  }
  snprintf(buf, sizeof(buf), "%s\n%s\n%s\n%s\n%s\n%s\n%s", ssid,
           up_wifi ? (String(WiFi.RSSI()) + " dBm").c_str() : "--",
           up_wifi ? WiFi.localIP().toString().c_str() : "--",
           batt, indoor, up, updated);
  lv_label_set_text(lbl_values, buf);
}

extern const DeskDashPlugin plugin_system = {"System", build, tick, nullptr};
