// Weather plugin: Open-Meteo forecast for LATITUDE/LONGITUDE in config.h.
// Other plugins (the clock) can read the latest data through weather_data().
#pragma once
#include "../../core/deskdash.h"

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

const Weather &weather_data();        // read on the UI thread
uint32_t weather_version();           // increments on every successful fetch
const char *weather_text(int code);
const char *weather_icon(int code, bool day);
lv_color_t weather_color(int code, bool day);

extern const DeskDashPlugin plugin_weather;
