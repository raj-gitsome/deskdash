#include "flights.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ---------------------------------------------------------------------------
//  Airline names for common callsign prefixes seen in South Florida
// ---------------------------------------------------------------------------
static const struct { const char *icao; const char *name; } AIRLINES[] = {
  {"AAL", "American"}, {"DAL", "Delta"}, {"UAL", "United"}, {"JBU", "JetBlue"},
  {"SWA", "Southwest"}, {"NKS", "Spirit"}, {"FFT", "Frontier"}, {"AAY", "Allegiant"},
  {"SCX", "Sun Country"}, {"ASA", "Alaska"}, {"BRE", "Breeze"}, {"ACA", "Air Canada"},
  {"WJA", "WestJet"}, {"TSC", "Air Transat"}, {"POE", "Porter"}, {"SIL", "Silver"},
  {"BHS", "Bahamasair"}, {"RPA", "Republic"}, {"ENY", "Envoy"}, {"EDV", "Endeavor"},
  {"SKW", "SkyWest"}, {"JIA", "PSA"}, {"PDT", "Piedmont"}, {"GJS", "GoJet"},
  {"EJA", "NetJets"}, {"LXJ", "Flexjet"}, {"XOJ", "XO"}, {"JSX", "JSX"},
  {"FDX", "FedEx"}, {"UPS", "UPS"}, {"GTI", "Atlas"}, {"VXP", "Avelo"},
};

static void make_display_name(const char *cs, char *out, size_t n) {
  size_t len = strlen(cs);
  if (len > 3 && isalpha(cs[0]) && isalpha(cs[1]) && isalpha(cs[2]) && isdigit(cs[3])) {
    for (auto &a : AIRLINES) {
      if (strncmp(cs, a.icao, 3) == 0) {
        snprintf(out, n, "%s %s", a.name, cs + 3);
        return;
      }
    }
  }
  snprintf(out, n, "%s", cs);
}

// ---------------------------------------------------------------------------
//  Route cache: one lookup per callsign, kept for 6 hours (adsb.lol rate-limits, so be gentle)
// ---------------------------------------------------------------------------
struct RouteEntry {
  char callsign[9];
  int8_t inbound;        // 1 = route ends here, 0 = goes elsewhere, -1 = no route published
  char origin[4];
  char origin_city[20];
  uint32_t stamp;        // millis() when looked up
};

static RouteEntry route_cache[160];

static RouteEntry *route_find(const char *cs) {
  for (auto &r : route_cache)
    if (r.callsign[0] && strcmp(r.callsign, cs) == 0 && millis() - r.stamp < 6UL * 3600 * 1000) return &r;
  return nullptr;
}

static RouteEntry *route_slot() {
  RouteEntry *oldest = &route_cache[0];
  for (auto &r : route_cache) {
    if (!r.callsign[0]) return &r;
    if (r.stamp < oldest->stamp) oldest = &r;
  }
  return oldest;
}

// ---------------------------------------------------------------------------
//  HTTP helpers
// ---------------------------------------------------------------------------
static bool http_json(const String &url, const String *post_body, JsonDocument &filter, JsonDocument &doc) {
  WiFiClientSecure client;
  client.setInsecure();      // public data; skips certificate checks
  HTTPClient http;          // HTTP/1.1: adsb.lol rejects 1.0 with "426 Upgrade Required"
  http.setTimeout(10000);
  http.setUserAgent("DeskDash/1.0 (ESP32-C5 desk display)");
  if (!http.begin(client, url)) return false;
  int status;
  if (post_body) {
    http.addHeader("Content-Type", "application/json");
    status = http.POST(*post_body);
  } else {
    status = http.GET();
  }
  if (status != HTTP_CODE_OK && status != HTTP_CODE_CREATED) {   // routeset answers 201
    Serial.printf("[flights] %s -> HTTP %d (%s)\n", url.c_str(), status, http.errorToString(status).c_str());
    http.end();
    return false;
  }
  // Read the whole body (40-80 KB, lands in PSRAM); getString() also undoes chunked encoding.
  String body = http.getString();
  http.end();
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[flights] JSON error: %s\n", err.c_str());
    return false;
  }
  return true;
}

static float angle_diff(float a, float b) {
  float d = fmodf(fabsf(a - b), 360.0f);
  return d > 180 ? 360 - d : d;
}

// ---------------------------------------------------------------------------
//  Main fetch
// ---------------------------------------------------------------------------
struct Candidate {
  char callsign[9];
  char type[5];
  char cat[3];           // ADS-B emitter category: A1 light, A2 small, A3 large, A7 rotorcraft...
  int alt;
  float gs, track, rate, lat, lon, dst, dir;
};

bool flights_fetch(const char *airport_icao, double lat, double lon, int radius_nm, Arrivals *out) {
  // 1. Aircraft near the airport
  JsonDocument filter;
  JsonObject f = filter["ac"][0].to<JsonObject>();
  for (const char *k : {"flight", "t", "category", "alt_baro", "gs", "track", "baro_rate", "geom_rate", "lat", "lon", "dst", "dir"})
    f[k] = true;

  JsonDocument doc;
  char url[96];
  snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/%d", lat, lon, radius_nm);
  if (!http_json(url, nullptr, filter, doc)) return false;

  static Candidate cand[60];
  int nc = 0, nearby = 0;
  JsonArray acs = doc["ac"];
  for (JsonObject a : acs) {
    nearby++;
    if (!a["alt_baro"].is<int>()) continue;          // "ground" or missing: not inbound
    const char *fl = a["flight"] | "";
    char cs[9] = {0};
    for (int i = 0, j = 0; fl[i] && j < 8; i++)
      if (fl[i] != ' ') cs[j++] = fl[i];
    if (!cs[0] || nc >= 60) continue;
    Candidate &c = cand[nc++];
    strcpy(c.callsign, cs);
    snprintf(c.type, sizeof(c.type), "%s", (const char *)(a["t"] | ""));
    snprintf(c.cat, sizeof(c.cat), "%s", (const char *)(a["category"] | ""));
    c.alt   = a["alt_baro"];
    c.gs    = a["gs"] | 0.0f;
    c.track = a["track"] | 0.0f;
    c.rate  = a["baro_rate"].is<int>() ? a["baro_rate"].as<float>() : (a["geom_rate"] | 0.0f);
    c.lat   = a["lat"] | 0.0f;
    c.lon   = a["lon"] | 0.0f;
    c.dst   = a["dst"] | 999.0f;
    c.dir   = a["dir"] | 0.0f;
  }
  doc.clear();
  // nearest first, so route lookups go to the aircraft most likely to be arriving
  for (int i = 0; i < nc; i++)
    for (int j = i + 1; j < nc; j++)
      if (cand[j].dst < cand[i].dst) { Candidate t = cand[i]; cand[i] = cand[j]; cand[j] = t; }

  // 2. Look up routes for callsigns we haven't seen yet (max 25 per request)
  JsonDocument req;
  JsonArray planes = req["planes"].to<JsonArray>();
  for (int i = 0; i < nc && planes.size() < 40; i++) {
    if (route_find(cand[i].callsign)) continue;
    JsonObject p = planes.add<JsonObject>();
    p["callsign"] = cand[i].callsign;
    p["lat"] = cand[i].lat;
    p["lng"] = cand[i].lon;
  }
  // adsb.lol allows 500 requests a day per address, so route lookups run at most every 10 minutes
  static uint32_t last_routeset_ms = 0;
  bool routes_due = last_routeset_ms == 0 || millis() - last_routeset_ms > 10UL * 60 * 1000;
  if (planes.size() > 0 && routes_due) {
    last_routeset_ms = millis();
    String body;
    serializeJson(req, body);
    JsonDocument rfilter;
    rfilter[0]["callsign"] = true;
    rfilter[0]["airport_codes"] = true;
    rfilter[0]["_airports"][0]["icao"] = true;
    rfilter[0]["_airports"][0]["iata"] = true;
    rfilter[0]["_airports"][0]["location"] = true;
    if (http_json("https://api.adsb.lol/api/0/routeset", &body, rfilter, doc)) {
      JsonArray routes = doc.as<JsonArray>();
      for (JsonObject r : routes) {
        RouteEntry *e = route_slot();
        memset(e, 0, sizeof(*e));
        snprintf(e->callsign, sizeof(e->callsign), "%s", (const char *)(r["callsign"] | ""));
        e->stamp = millis();
        const char *codes = r["airport_codes"] | "unknown";
        if (strcmp(codes, "unknown") == 0) {
          e->inbound = -1;
          continue;
        }
        // "KBOS-KPBI" or multi-leg "KBOS-KPBI-KMIA": inbound if our airport appears after the first code
        const char *hit = strstr(codes, airport_icao);
        e->inbound = (hit && hit != codes) ? 1 : 0;
        if (e->inbound) {
          char prev[5] = {0};
          memcpy(prev, hit - 5, 4);   // the ICAO code just before "-KPBI"
          JsonArray aps = r["_airports"];
          for (JsonObject ap : aps) {
            if (strcmp(ap["icao"] | "", prev) == 0) {
              snprintf(e->origin, sizeof(e->origin), "%s", (const char *)(ap["iata"] | ""));
              snprintf(e->origin_city, sizeof(e->origin_city), "%s", (const char *)(ap["location"] | ""));
            }
          }
        }
      }
    }
    doc.clear();
  }

  // 3. Pick arrivals
  Arrivals res;
  res.nearby = nearby;
  static Arrival all[60];
  int na = 0;
  for (int i = 0; i < nc; i++) {
    Candidate &c = cand[i];
    RouteEntry *r = route_find(c.callsign);
    bool inbound;
    if (r && r->inbound >= 0) {
      inbound = r->inbound == 1;
    } else {
      // No published route: judge by the flight path. Descending and low enough for its
      // distance (about a 3.5 degree path plus margin), which filters out jets passing over
      // on their way to FLL or MIA. Far out it must be heading roughly at the airport; close
      // in it may be on downwind or base, so any track not pointing away counts.
      // Light aircraft and helicopters are left out: most are training flights nearby.
      float to_airport = fmodf(c.dir + 180.0f, 360.0f);   // "dir" is the bearing from the airport to the aircraft
      float off = angle_diff(c.track, to_airport);
      bool light = strcmp(c.cat, "A1") == 0 || strcmp(c.cat, "A7") == 0 || c.cat[0] == 'B';
      bool descending = c.rate < -100;
      bool close_in = c.dst < 15 && c.alt < 5000 && descending && off < 90;
      bool on_profile = c.dst < 45 && c.alt < 18000 && c.alt <= 400 * c.dst + 3000 && descending && off < 45;
      inbound = !light && (close_in || on_profile);
    }
    if (!inbound) continue;
    Arrival &a = all[na++];
    memset(&a, 0, sizeof(a));
    strcpy(a.callsign, c.callsign);
    make_display_name(c.callsign, a.display, sizeof(a.display));
    strcpy(a.type, c.type);
    if (r && r->inbound == 1) {
      strcpy(a.origin, r->origin);
      strcpy(a.origin_city, r->origin_city);
    }
    a.dist_nm = c.dst;
    a.alt_ft = c.alt;
    a.eta_min = c.gs > 40 ? (int)(c.dst / c.gs * 60.0f + 0.5f) : -1;
    a.eta = a.eta_min >= 0 ? time(nullptr) + a.eta_min * 60 : 0;
  }
  // nearest ETA first (unknown ETA last)
  for (int i = 0; i < na; i++)
    for (int j = i + 1; j < na; j++) {
      int ei = all[i].eta_min < 0 ? 9999 : all[i].eta_min, ej = all[j].eta_min < 0 ? 9999 : all[j].eta_min;
      if (ej < ei) { Arrival t = all[i]; all[i] = all[j]; all[j] = t; }
    }
  for (int i = 0; i < na && res.n < MAX_ARRIVALS; i++) res.items[res.n++] = all[i];
  res.updated = time(nullptr);
  *out = res;
  Serial.printf("[flights] %d aircraft nearby, %d arriving\n", nearby, na);
  return true;
}
