// Weather page: conditions, high/low, feels-like, humidity, wind and the next 4 hours.
#include "weather.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static Weather weather;
static uint32_t version = 0;

static lv_obj_t *lbl_icon, *lbl_temp, *lbl_cond, *lbl_hilo, *lbl_detail;
static lv_obj_t *lbl_h_time[4], *lbl_h_icon[4], *lbl_h_temp[4];

const Weather &weather_data() { return weather; }
uint32_t weather_version() { return version; }

// ---- WMO weather codes (used by Open-Meteo) -------------------------------------------
const char *weather_text(int code) {
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

const char *weather_icon(int code, bool day) {
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

lv_color_t weather_color(int code, bool day) {
  if (code <= 2) return day ? COL_SUN : COL_MOON;
  if (code >= 95) return COL_STORM;
  if (code >= 51) return COL_RAIN;
  return COL_CLOUD;
}

// ---- Page ------------------------------------------------------------------------------
static void build(lv_obj_t *p) {
  make_label(p, &font_text_24, COL_MUTED, LV_ALIGN_TOP_MID, 0, 22, LOCATION_NAME);

  lv_obj_t *row = make_row(p, 62, 18);
  lbl_icon = make_label(row, &font_icons_96, COL_CLOUD, LV_ALIGN_DEFAULT, 0, 0, ICON_CLOUD);
  lbl_temp = make_label(row, &font_text_56, COL_TEXT, LV_ALIGN_DEFAULT, 0, 0, "--");

  lbl_cond   = make_label(p, &font_text_24, COL_TEXT,  LV_ALIGN_TOP_MID, 0, 170, "--");
  lbl_hilo   = make_label(p, &font_text_24, COL_MUTED, LV_ALIGN_TOP_MID, 0, 204, "");
  lbl_detail = make_label(p, &font_text_24, COL_MUTED, LV_ALIGN_TOP_MID, 0, 238, "");

  make_divider(p, 284, 260);

  for (int i = 0; i < 4; i++) {   // next four hours, 80 px columns
    lv_coord_t x = 40 + i * 80 - SCREEN_W / 2;
    lbl_h_time[i] = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, x, 300, "");
    lbl_h_icon[i] = make_label(p, &font_icons_44, COL_CLOUD, LV_ALIGN_TOP_MID, x, 330, "");
    lbl_h_temp[i] = make_label(p, &font_text_24, COL_TEXT, LV_ALIGN_TOP_MID, x, 386, "");
  }
}

static void show() {
  if (!weather.valid) return;
  char buf[96];
  snprintf(buf, sizeof(buf), "%.0f" DEG, weather.temp);
  lv_label_set_text(lbl_temp, buf);
  lv_label_set_text(lbl_icon, weather_icon(weather.code, weather.is_day));
  lv_obj_set_style_text_color(lbl_icon, weather_color(weather.code, weather.is_day), 0);
  lv_label_set_text(lbl_cond, weather_text(weather.code));

  snprintf(buf, sizeof(buf), "H %.0f" DEG "   L %.0f" DEG "   " ICON_THERMO " %.0f" DEG,
           weather.hi, weather.lo, weather.feels);
  lv_label_set_text(lbl_hilo, buf);
  snprintf(buf, sizeof(buf), ICON_TINT " %d%%      " ICON_WIND " %.0f %s",
           weather.humidity, weather.wind, USE_FAHRENHEIT ? "mph" : "km/h");
  lv_label_set_text(lbl_detail, buf);

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

// ---- Network ---------------------------------------------------------------------------
static bool fetch() {
  char url[512];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,wind_speed_10m"
           "&hourly=temperature_2m,weather_code,is_day"
           "&daily=temperature_2m_max,temperature_2m_min"
           "&temperature_unit=%s&wind_speed_unit=%s&timezone=auto&forecast_days=2",
           (double)LATITUDE, (double)LONGITUDE,
           USE_FAHRENHEIT ? "fahrenheit" : "celsius", USE_FAHRENHEIT ? "mph" : "kmh");

  WiFiClient client;          // plain HTTP: public, read-only data
  HTTPClient http;
  http.useHTTP10(true);       // no chunked encoding, so the JSON parser can read the stream
  http.setTimeout(10000);
  if (!http.begin(client, url)) return false;
  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("[weather] HTTP %d (%s)\n", status, http.errorToString(status).c_str());
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
    version++;
    show();
    ui_unlock();
  }
  Serial.printf("[weather] %.1f, code %d, %s\n", w.temp, w.code, weather_text(w.code));
  return true;
}

static void poll() {
  static uint32_t last = 0;
  static bool ok_once = false;
  if (!dd_wifi_up()) return;
  uint32_t interval = (ok_once ? WEATHER_REFRESH_MINUTES * 60UL : 30UL) * 1000UL;
  if (last == 0 || millis() - last > interval) {
    last = millis();
    if (fetch()) ok_once = true;
  }
}

extern const DeskDashPlugin plugin_weather = {"Weather", build, nullptr, poll};

#ifdef DESKDASH_PREVIEW
void weather_preview_set(const Weather &w) { weather = w; version++; show(); }
#endif
