// ============================================================================
//  DeskDash — your settings
//  Edit the values below, then upload. Nothing else needs changing.
// ============================================================================
#pragma once

// ---- Wi-Fi -----------------------------------------------------------------
// The ESP32-C5 supports 2.4 GHz and 5 GHz networks. The name is case-sensitive.
#define WIFI_SSID      "YOUR_WIFI_NAME"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"

// ---- Location (used for weather) -------------------------------------------
#define LOCATION_NAME  "Anytown, USA"
#define LATITUDE       0.0     // your latitude, e.g. 40.7128
#define LONGITUDE      0.0     // your longitude, e.g. -74.0060 (west is negative)

// ---- Time ------------------------------------------------------------------
// POSIX time zone string. US Eastern with daylight saving:
#define TIMEZONE       "EST5EDT,M3.2.0,M11.1.0"
// Other examples: Central "CST6CDT,M3.2.0,M11.1.0", Pacific "PST8PDT,M3.2.0,M11.1.0"
#define USE_24_HOUR    false

// ---- Units -----------------------------------------------------------------
#define USE_FAHRENHEIT true      // false = Celsius and km/h

// ---- Weather refresh -------------------------------------------------------
#define WEATHER_REFRESH_MINUTES 15

// ---- Screen brightness (percent) -------------------------------------------
#define DAY_BRIGHTNESS     80
#define NIGHT_BRIGHTNESS   10    // used between the hours below; tap to wake
#define NIGHT_START_HOUR   22    // 10 PM
#define NIGHT_END_HOUR     7     // 7 AM
#define WAKE_SECONDS       30    // how long a tap brightens the screen at night
