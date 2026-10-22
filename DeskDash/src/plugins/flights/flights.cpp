#include "flights.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "../../core/net.h"
#include <esp32-hal-psram.h>

// ============================================================================
//  Airports offered in the on-screen picker. Add your own here.
// ============================================================================
static const Airport AIRPORTS[] = {
  {"PBI", "KPBI", "Palm Beach",       26.6832f,  -80.0956f},
  {"FLL", "KFLL", "Fort Lauderdale",  26.0726f,  -80.1527f},
  {"MIA", "KMIA", "Miami",            25.7932f,  -80.2906f},
  {"BCT", "KBCT", "Boca Raton",       26.3785f,  -80.1077f},
  {"MCO", "KMCO", "Orlando",          28.4294f,  -81.3090f},
  {"TPA", "KTPA", "Tampa",            27.9755f,  -82.5332f},
  {"RSW", "KRSW", "Fort Myers",       26.5362f,  -81.7552f},
  {"SRQ", "KSRQ", "Sarasota",         27.3954f,  -82.5544f},
  {"JAX", "KJAX", "Jacksonville",     30.4941f,  -81.6879f},
  {"ATL", "KATL", "Atlanta",          33.6367f,  -84.4281f},
  {"CLT", "KCLT", "Charlotte",        35.2140f,  -80.9431f},
  {"BNA", "KBNA", "Nashville",        36.1245f,  -86.6782f},
  {"DCA", "KDCA", "Washington DCA",   38.8521f,  -77.0377f},
  {"IAD", "KIAD", "Washington IAD",   38.9445f,  -77.4558f},
  {"PHL", "KPHL", "Philadelphia",     39.8719f,  -75.2411f},
  {"EWR", "KEWR", "Newark",           40.6925f,  -74.1687f},
  {"JFK", "KJFK", "New York JFK",     40.6398f,  -73.7789f},
  {"LGA", "KLGA", "New York LGA",     40.7772f,  -73.8726f},
  {"BOS", "KBOS", "Boston",           42.3643f,  -71.0052f},
  {"DTW", "KDTW", "Detroit",          42.2124f,  -83.3534f},
  {"ORD", "KORD", "Chicago O'Hare",   41.9786f,  -87.9048f},
  {"MSP", "KMSP", "Minneapolis",      44.8820f,  -93.2218f},
  {"DFW", "KDFW", "Dallas/Ft Worth",  32.8968f,  -97.0380f},
  {"IAH", "KIAH", "Houston",          29.9844f,  -95.3414f},
  {"AUS", "KAUS", "Austin",           30.1945f,  -97.6699f},
  {"DEN", "KDEN", "Denver",           39.8561f, -104.6737f},
  {"PHX", "KPHX", "Phoenix",          33.4343f, -112.0116f},
  {"LAS", "KLAS", "Las Vegas",        36.0840f, -115.1537f},
  {"SAN", "KSAN", "San Diego",        32.7336f, -117.1897f},
  {"LAX", "KLAX", "Los Angeles",      33.9425f, -118.4081f},
  {"SFO", "KSFO", "San Francisco",    37.6190f, -122.3750f},
  {"SEA", "KSEA", "Seattle",          47.4490f, -122.3093f},
};
static const int AIRPORT_COUNT = sizeof(AIRPORTS) / sizeof(AIRPORTS[0]);

static volatile int airport_idx = 0;
static volatile bool refetch_now = false;
static FlightBoard board;
static uint32_t version = 1;

int flights_airport_count() { return AIRPORT_COUNT; }
const Airport &flights_airport_at(int i) { return AIRPORTS[i]; }
const Airport &flights_airport() { return AIRPORTS[airport_idx]; }
const FlightBoard &flights_board() { return board; }
uint32_t flights_version() { return version; }

static void load_airport() {
  static bool loaded = false;
  if (loaded) return;
  loaded = true;
  String code = dd_setting_get("airport", AIRPORT_CODE);
  for (int i = 0; i < AIRPORT_COUNT; i++)
    if (code.equalsIgnoreCase(AIRPORTS[i].iata)) airport_idx = i;
}

// ============================================================================
//  Airline names for common callsign prefixes
// ============================================================================
static const struct { const char *icao; const char *name; } AIRLINES[] = {
  {"AAL", "American"}, {"DAL", "Delta"}, {"UAL", "United"}, {"JBU", "JetBlue"},
  {"SWA", "Southwest"}, {"NKS", "Spirit"}, {"FFT", "Frontier"}, {"AAY", "Allegiant"},
  {"SCX", "Sun Country"}, {"ASA", "Alaska"}, {"BRE", "Breeze"}, {"ACA", "Air Canada"},
  {"WJA", "WestJet"}, {"TSC", "Air Transat"}, {"POE", "Porter"}, {"SIL", "Silver"},
  {"BHS", "Bahamasair"}, {"RPA", "Republic"}, {"ENY", "Envoy"}, {"EDV", "Endeavor"},
  {"SKW", "SkyWest"}, {"JIA", "PSA"}, {"PDT", "Piedmont"}, {"GJS", "GoJet"},
  {"EJA", "NetJets"}, {"LXJ", "Flexjet"}, {"XOJ", "XO"}, {"JSX", "JSX"},
  {"FDX", "FedEx"}, {"UPS", "UPS"}, {"GTI", "Atlas"}, {"VXP", "Avelo"},
  {"BAW", "British"}, {"VIR", "Virgin"}, {"AFR", "Air France"}, {"DLH", "Lufthansa"},
};

static void make_display_name(const char *cs, char *out, size_t n) {
  if (strlen(cs) > 3 && isalpha(cs[0]) && isalpha(cs[1]) && isalpha(cs[2]) && isdigit(cs[3])) {
    for (auto &a : AIRLINES) {
      if (strncmp(cs, a.icao, 3) == 0) {
        snprintf(out, n, "%s %s", a.name, cs + 3);
        return;
      }
    }
  }
  snprintf(out, n, "%s", cs);
}

// ============================================================================
//  Route cache (PSRAM): one lookup per callsign, kept for 6 hours
// ============================================================================
struct RouteAirport {
  char icao[5], iata[4], city[20];
};

struct RouteEntry {
  char callsign[9];
  bool known;             // false = adsb.lol has no route for this callsign
  char codes[48];         // "KBOS-KPBI" or multi-leg "KBOS-KPBI-KMIA"
  RouteAirport ap[4];
  uint32_t stamp;
};

static const int ROUTE_CACHE_SIZE = 200;
static RouteEntry *route_cache = nullptr;

static RouteEntry *route_find(const char *cs) {
  if (!route_cache) return nullptr;
  for (int i = 0; i < ROUTE_CACHE_SIZE; i++) {
    RouteEntry &r = route_cache[i];
    if (r.callsign[0] && strcmp(r.callsign, cs) == 0 && millis() - r.stamp < 6UL * 3600 * 1000) return &r;
  }
  return nullptr;
}

static RouteEntry *route_slot() {
  RouteEntry *oldest = &route_cache[0];
  for (int i = 0; i < ROUTE_CACHE_SIZE; i++) {
    if (!route_cache[i].callsign[0]) return &route_cache[i];
    if (route_cache[i].stamp < oldest->stamp) oldest = &route_cache[i];
  }
  return oldest;
}

// Position of `icao` in the route ("KBOS-KPBI-KMIA" -> KPBI is 1 of 3), or -1
static int route_position(const RouteEntry &r, const char *icao, int *legs) {
  *legs = 0;
  int pos = -1;
  const char *p = r.codes;
  while (*p) {
    if (strncmp(p, icao, 4) == 0 && (p[4] == '-' || p[4] == 0)) pos = *legs;
    (*legs)++;
    const char *dash = strchr(p, '-');
    if (!dash) break;
    p = dash + 1;
  }
  return pos;
}

// IATA + city for the code at `index` in the route
static void route_airport(const RouteEntry &r, int index, char *iata, char *city) {
  iata[0] = city[0] = 0;
  const char *p = r.codes;
  for (int i = 0; i < index && p; i++) {
    p = strchr(p, '-');
    if (p) p++;
  }
  if (!p) return;
  for (auto &a : r.ap) {
    if (a.icao[0] && strncmp(p, a.icao, 4) == 0) {
      strcpy(iata, a.iata);
      strcpy(city, a.city);
      return;
    }
  }
}

// ============================================================================
//  HTTP
// ============================================================================
static bool http_json(const String &url, const String *post_body, JsonDocument &filter, JsonDocument &doc) {
  WiFiClientSecure client;
  client.setInsecure();      // public data; skips certificate checks
  HTTPClient http;           // HTTP/1.1: adsb.lol rejects 1.0 with "426 Upgrade Required"
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
  if (status != HTTP_CODE_OK && status != HTTP_CODE_CREATED) {
    Serial.printf("[flights] %s -> HTTP %d (%s)\n", url.c_str(), status, http.errorToString(status).c_str());
    http.end();
    return false;
  }
  PsramBody body;                    // 40-80 KB: kept in PSRAM so Wi-Fi keeps its internal RAM
  bool got = dd_read_body(http, body);
  http.end();
  if (!got) return false;
  DeserializationError err = deserializeJson(doc, body.data, body.len, DeserializationOption::Filter(filter));
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

// ============================================================================
//  Fetch and classify
// ============================================================================
// Arrivals followed from fetch to fetch, so a flight can be shown as "Landed".
struct Tracked {
  Flight f;
  time_t last_seen;
  float last_dst;
  int last_alt;
};
static const int MAX_TRACKED = 40;
static Tracked tracked[MAX_TRACKED];
static int n_tracked = 0;
static int tracked_airport = -1;

static Tracked *track_find(const char *cs) {
  for (int i = 0; i < n_tracked; i++)
    if (strcmp(tracked[i].f.callsign, cs) == 0) return &tracked[i];
  return nullptr;
}

static Tracked *track_add(const Flight &f, time_t now) {
  Tracked *t = track_find(f.callsign);
  if (!t) {
    if (n_tracked < MAX_TRACKED) {
      t = &tracked[n_tracked++];
    } else {                       // full: reuse the oldest landed entry, else the stalest one
      t = &tracked[0];
      for (int i = 1; i < n_tracked; i++) {
        Tracked &c = tracked[i];
        if ((c.f.landed && !t->f.landed) || (c.f.landed == t->f.landed && c.last_seen < t->last_seen)) t = &c;
      }
    }
    memset(t, 0, sizeof(*t));
  }
  char other[4], city[20];         // keep the origin if this fetch has no route for it
  strcpy(other, t->f.other); strcpy(city, t->f.other_city);
  t->f = f;
  if (!t->f.other[0]) { strcpy(t->f.other, other); strcpy(t->f.other_city, city); }
  t->last_seen = now;
  t->last_dst = f.dist_nm;
  t->last_alt = f.alt_ft;
  return t;
}

static void track_remove(int i) { tracked[i] = tracked[--n_tracked]; }

static void mark_landed(Tracked &t, time_t at) {
  if (t.f.landed) return;
  t.f.landed = true;
  t.f.when = at;
  t.f.dist_nm = 0;
  t.f.alt_ft = 0;
  Serial.printf("[flights] %s landed\n", t.f.callsign);
}

struct Candidate {
  char callsign[9], type[5], cat[3];
  bool ground;
  int alt;
  float gs, track, rate, lat, lon, dst, dir;
};

static void sort_by_when(Flight *f, int n) {
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++) {
      time_t a = f[i].when ? f[i].when : (time_t)0x7FFFFFFF, b = f[j].when ? f[j].when : (time_t)0x7FFFFFFF;
      if (b < a) { Flight t = f[i]; f[i] = f[j]; f[j] = t; }
    }
}

static bool fetch() {
  const int idx = airport_idx;
  const Airport ap = AIRPORTS[idx];

  // 1. Aircraft near the airport
  JsonDocument filter;
  JsonObject f = filter["ac"][0].to<JsonObject>();
  for (const char *k : {"flight", "t", "category", "alt_baro", "gs", "track", "baro_rate", "geom_rate",
                        "lat", "lon", "dst", "dir"})
    f[k] = true;
  JsonDocument doc(dd_psram_json());
  char url[112];
  snprintf(url, sizeof(url), "https://opendata.adsb.fi/api/v3/lat/%.4f/lon/%.4f/dist/%d",
           ap.lat, ap.lon, ARRIVALS_RADIUS_NM);
  if (!http_json(url, nullptr, filter, doc)) return false;

  static Candidate cand[80];
  int nc = 0, nearby = 0;
  JsonArray acs = doc["ac"];
  for (JsonObject a : acs) {
    nearby++;
    bool ground = !a["alt_baro"].is<int>();
    float dst = a["dst"] | 999.0f;
    if (ground && dst > 3) continue;                 // only taxiing traffic at this airport
    const char *fl = a["flight"] | "";
    char cs[9] = {0};
    for (int i = 0, j = 0; fl[i] && j < 8; i++)
      if (fl[i] != ' ') cs[j++] = fl[i];
    if (!cs[0] || nc >= 80) continue;
    Candidate &c = cand[nc++];
    strcpy(c.callsign, cs);
    snprintf(c.type, sizeof(c.type), "%s", (const char *)(a["t"] | ""));
    snprintf(c.cat, sizeof(c.cat), "%s", (const char *)(a["category"] | ""));
    c.ground = ground;
    c.alt   = ground ? 0 : a["alt_baro"].as<int>();
    c.gs    = a["gs"] | 0.0f;
    c.track = a["track"] | 0.0f;
    c.rate  = a["baro_rate"].is<int>() ? a["baro_rate"].as<float>() : (a["geom_rate"] | 0.0f);
    c.lat   = a["lat"] | 0.0f;
    c.lon   = a["lon"] | 0.0f;
    c.dst   = dst;
    c.dir   = a["dir"] | 0.0f;
  }
  doc.clear();
  for (int i = 0; i < nc; i++)       // nearest first, so route lookups go to the likeliest flights
    for (int j = i + 1; j < nc; j++)
      if (cand[j].dst < cand[i].dst) { Candidate t = cand[i]; cand[i] = cand[j]; cand[j] = t; }

  // 2. Route lookups for new callsigns, at most every 10 minutes (rate limit)
  if (!route_cache) route_cache = (RouteEntry *)ps_calloc(ROUTE_CACHE_SIZE, sizeof(RouteEntry));
  static uint32_t last_routes = 0;
  if (route_cache && (last_routes == 0 || millis() - last_routes > 10UL * 60 * 1000)) {
    JsonDocument req(dd_psram_json());
    JsonArray planes = req["planes"].to<JsonArray>();
    for (int i = 0; i < nc && planes.size() < 40; i++) {
      if (route_find(cand[i].callsign)) continue;
      JsonObject p = planes.add<JsonObject>();
      p["callsign"] = cand[i].callsign;
      p["lat"] = cand[i].lat;
      p["lng"] = cand[i].lon;
    }
    if (planes.size() > 0) {
      last_routes = millis();
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
          e->known = strcmp(codes, "unknown") != 0;
          if (!e->known) continue;
          snprintf(e->codes, sizeof(e->codes), "%s", codes);
          int k = 0;
          JsonArray aps = r["_airports"];
          for (JsonObject a : aps) {
            if (k >= 4) break;
            snprintf(e->ap[k].icao, sizeof(e->ap[k].icao), "%s", (const char *)(a["icao"] | ""));
            snprintf(e->ap[k].iata, sizeof(e->ap[k].iata), "%s", (const char *)(a["iata"] | ""));
            snprintf(e->ap[k].city, sizeof(e->ap[k].city), "%s", (const char *)(a["location"] | ""));
            k++;
          }
        }
      }
      doc.clear();
    }
  }

  // 3. Classify
  FlightBoard nb;
  nb.nearby = nearby;
  static Flight arr[80], dep[80];
  int na = 0, nd = 0;
  time_t now = time(nullptr);
  if (tracked_airport != idx) { n_tracked = 0; tracked_airport = idx; }
  for (int i = 0; i < nc; i++) {
    Candidate &c = cand[i];
    RouteEntry *r = route_find(c.callsign);
    Tracked *tr = track_find(c.callsign);
    int legs = 0, pos = (r && r->known) ? route_position(*r, ap.icao, &legs) : -1;
    bool light = strcmp(c.cat, "A1") == 0 || strcmp(c.cat, "A7") == 0 || c.cat[0] == 'B';
    float to_airport = fmodf(c.dir + 180.0f, 360.0f);   // "dir" = bearing from the airport to the aircraft
    float off_in = angle_diff(c.track, to_airport), off_out = angle_diff(c.track, c.dir);

    bool is_arr = false, is_dep = false;
    if (pos >= 0) {
      // Route touches this airport: first leg = departure, last = arrival, middle = by flight path
      if (pos == 0) is_dep = true;
      else if (pos == legs - 1) is_arr = !c.ground;
      else if (c.ground || c.rate > 0) is_dep = true;
      else is_arr = true;
      if (is_dep && !c.ground && off_out > 100) is_dep = false;   // route says here, but it's heading in
    } else if (!(r && r->known) && !light && !c.ground) {
      // No published route: judge by flight path, low enough for its distance (~3.5 degree slope)
      bool on_slope = c.alt < 18000 && c.alt <= 400 * c.dst + 3000;
      bool close_in = c.dst < 15 && c.alt < 5000;
      is_arr = c.rate < -100 && ((close_in && off_in < 90) || (on_slope && c.dst < 45 && off_in < 45));
      is_dep = c.rate > 300 && c.dst < 30 && c.alt <= 400 * c.dst + 5000 && off_out < 45;
    }
    // Touchdown: a tracked arrival now on the ground here, or a routed arrival taxiing in
    if (c.ground && (tr || (pos >= 0 && pos == legs - 1 && c.gs > 2))) {
      if (!tr) {
        Flight lf;
        memset(&lf, 0, sizeof(lf));
        strcpy(lf.callsign, c.callsign);
        make_display_name(c.callsign, lf.display, sizeof(lf.display));
        strcpy(lf.type, c.type);
        route_airport(*r, pos - 1, lf.other, lf.other_city);
        tr = track_add(lf, now);
      }
      tr->last_seen = now;
      mark_landed(*tr, now);
      continue;
    }
    // A tracked arrival the heuristic lost for a moment (level-off, go-around): keep following it
    if (!is_arr && !is_dep && tr && !tr->f.landed && !c.ground) {
      tr->last_seen = now;
      tr->last_dst = tr->f.dist_nm = c.dst;
      tr->last_alt = tr->f.alt_ft = c.alt;
      continue;
    }
    if (!is_arr && !is_dep) continue;
    if (is_dep && tr && !tr->f.landed) track_remove(tr - tracked);   // misjudged earlier

    Flight &fl = is_arr ? arr[na++] : dep[nd++];
    memset(&fl, 0, sizeof(fl));
    strcpy(fl.callsign, c.callsign);
    make_display_name(c.callsign, fl.display, sizeof(fl.display));
    strcpy(fl.type, c.type);
    fl.dist_nm = c.dst;
    fl.alt_ft = c.alt;
    fl.on_ground = c.ground;
    if (pos >= 0) route_airport(*r, is_arr ? pos - 1 : pos + 1, fl.other, fl.other_city);
    if (c.gs > 40 && !c.ground) {
      int minutes = (int)(c.dst / c.gs * 60.0f + 0.5f);
      fl.when = is_arr ? now + minutes * 60 : now - minutes * 60;   // arrival ETA, or roughly when it took off
    }
  }
  for (int i = 0; i < na; i++) {
    Tracked *t = track_find(arr[i].callsign);
    if (t && t->f.landed) continue;          // already down; ignore a stray airborne report
    track_add(arr[i], now);
  }

  // Arrivals that dropped out of coverage: low and close means they landed (receivers
  // rarely see aircraft on the runway); otherwise give them 10 minutes, then drop them.
  for (int i = n_tracked - 1; i >= 0; i--) {
    Tracked &t = tracked[i];
    if (t.f.landed) {
      if (now - t.f.when > LANDED_KEEP_MINUTES * 60L) track_remove(i);
      continue;
    }
    if (t.last_seen == now) continue;
    if (t.last_dst < 8 && t.last_alt < 3000) {
      time_t at = t.f.when ? t.f.when : t.last_seen;
      if (at < t.last_seen) at = t.last_seen;
      if (at > now) at = now;
      mark_landed(t, at);
    } else if (now - t.last_seen > 600) {
      track_remove(i);
    }
  }

  // Arrivals list: still inbound (soonest first), then landed (most recent first)
  na = 0;
  int n_landed = 0;
  for (int i = 0; i < n_tracked; i++) if (!tracked[i].f.landed) arr[na++] = tracked[i].f;
  sort_by_when(arr, na);
  for (int i = 0; i < n_tracked; i++) if (tracked[i].f.landed) { arr[na + n_landed++] = tracked[i].f; }
  for (int i = na; i < na + n_landed; i++)
    for (int j = i + 1; j < na + n_landed; j++)
      if (arr[j].when > arr[i].when) { Flight t = arr[i]; arr[i] = arr[j]; arr[j] = t; }
  // departures: taxiing first, then most recent takeoff first
  for (int i = 0; i < nd; i++)
    for (int j = i + 1; j < nd; j++) {
      bool swap = (dep[j].on_ground && !dep[i].on_ground) ||
                  (dep[j].on_ground == dep[i].on_ground && dep[j].when > dep[i].when);
      if (swap) { Flight t = dep[i]; dep[i] = dep[j]; dep[j] = t; }
    }
  for (int i = 0; i < na + n_landed && nb.n_arr < MAX_FLIGHTS; i++) nb.arr[nb.n_arr++] = arr[i];
  for (int i = 0; i < nd && nb.n_dep < MAX_FLIGHTS; i++) nb.dep[nb.n_dep++] = dep[i];
  nb.updated = now;

  if (ui_lock()) {
    if (idx == airport_idx) {    // skip if the airport was changed while this fetch ran
      board = nb;
      version++;
    }
    ui_unlock();
  }
  Serial.printf("[flights] %s: %d aircraft nearby, %d arriving, %d landed, %d departing\n", ap.iata, nearby, na, n_landed, nd);
  return true;
}

void flights_poll() {
  load_airport();
  static uint32_t last = 0;
  if (!dd_wifi_up()) return;
  uint32_t interval = (is_night() ? ARRIVALS_NIGHT_REFRESH_SECONDS : ARRIVALS_REFRESH_SECONDS) * 1000UL;
  if (refetch_now || last == 0 || millis() - last > interval) {
    refetch_now = false;
    last = millis();
    fetch();
  }
}

// ============================================================================
//  Airport picker (UI thread)
// ============================================================================
static lv_obj_t *picker = nullptr;
static lv_obj_t *roller = nullptr;

static void close_picker() {
  if (picker) lv_obj_del(picker);
  picker = roller = nullptr;
}

static void picker_save(lv_event_t *e) {
  int idx = lv_roller_get_selected(roller);
  close_picker();
  if (idx == airport_idx) return;
  airport_idx = idx;
  dd_setting_set("airport", AIRPORTS[idx].iata);
  board = FlightBoard();       // clear the old airport's flights (tracking resets on next fetch)
  version++;
  refetch_now = true;
  Serial.printf("[flights] airport set to %s\n", AIRPORTS[idx].iata);
}

static void picker_cancel(lv_event_t *e) { close_picker(); }

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_color_t fg,
                             lv_align_t align, lv_coord_t x, lv_event_cb_t cb) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, 128, 52);
  lv_obj_align(b, align, x, -32);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_radius(b, 14, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l = make_label(b, &font_text_24, fg, LV_ALIGN_CENTER, 0, 0, text);
  (void)l;
  return b;
}

static void open_picker(lv_event_t *e) {
  if (picker) return;
  picker = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(picker);
  lv_obj_set_size(picker, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(picker, COL_BG, 0);
  lv_obj_set_style_bg_opa(picker, LV_OPA_COVER, 0);
  lv_obj_add_flag(picker, LV_OBJ_FLAG_CLICKABLE);    // swallow touches meant for the page below

  make_label(picker, &font_text_24, COL_TEXT, LV_ALIGN_TOP_MID, 0, 28, "Choose airport");
  make_label(picker, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, 0, 62, "Scroll, then tap Use");

  static String options;        // roller keeps a copy, but build it once
  options = "";
  for (int i = 0; i < AIRPORT_COUNT; i++) {
    if (i) options += "\n";
    options += AIRPORTS[i].iata;
    options += "  ";
    options += AIRPORTS[i].name;
  }
  roller = lv_roller_create(picker);
  lv_roller_set_options(roller, options.c_str(), LV_ROLLER_MODE_NORMAL);
  lv_roller_set_visible_row_count(roller, 7);
  lv_obj_set_width(roller, 280);
  lv_obj_align(roller, LV_ALIGN_TOP_MID, 0, 96);
  lv_obj_set_style_text_font(roller, &font_text_24, 0);
  lv_obj_set_style_text_font(roller, &font_text_24, LV_PART_SELECTED);
  lv_obj_set_style_text_line_space(roller, 14, 0);
  lv_obj_set_style_bg_color(roller, COL_PANEL, 0);
  lv_obj_set_style_text_color(roller, COL_MUTED, 0);
  lv_obj_set_style_border_width(roller, 0, 0);
  lv_obj_set_style_bg_color(roller, COL_ACCENT, LV_PART_SELECTED);
  lv_obj_set_style_text_color(roller, COL_BG, LV_PART_SELECTED);
  lv_roller_set_selected(roller, airport_idx, LV_ANIM_OFF);

  make_button(picker, "Cancel", COL_FAINT, COL_TEXT, LV_ALIGN_BOTTOM_LEFT, 22, picker_cancel);
  make_button(picker, "Use", COL_ACCENT, COL_BG, LV_ALIGN_BOTTOM_RIGHT, -22, picker_save);
}

// ============================================================================
//  Page UI shared by Arrivals and Departures
// ============================================================================
void flights_build_page(FlightPage &pg, lv_obj_t *p, bool arrivals) {
  load_airport();
  pg.arrivals = arrivals;
  pg.title = make_label(p, &font_text_24, COL_TEXT, LV_ALIGN_TOP_MID, 0, 22, "");
  lv_obj_add_flag(pg.title, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(pg.title, 16);
  lv_obj_add_event_cb(pg.title, open_picker, LV_EVENT_CLICKED, NULL);

  pg.status = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, 0, 54, "");
  lv_label_set_long_mode(pg.status, LV_LABEL_LONG_DOT);
  lv_obj_set_width(pg.status, 290);
  lv_obj_set_style_text_align(pg.status, LV_TEXT_ALIGN_CENTER, 0);
  pg.empty  = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, 0, 200, "Looking for flights...");
  for (int i = 0; i < MAX_FLIGHTS; i++) {
    lv_coord_t y = 90 + i * 57;
    pg.name[i] = make_label(p, &font_text_24, COL_TEXT, LV_ALIGN_TOP_LEFT, 20, y, "");
    lv_label_set_long_mode(pg.name[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(pg.name[i], 190);
    pg.time[i] = make_label(p, &font_text_24, arrivals ? COL_ACCENT : COL_GOOD, LV_ALIGN_TOP_RIGHT, -20, y, "");
    pg.info[i] = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_LEFT, 20, y + 28, "");
    lv_label_set_long_mode(pg.info[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(pg.info[i], 282);
  }
}

void flights_tick_page(FlightPage &pg) {
  if (pg.shown == version) return;
  pg.shown = version;
  char buf[96];
  const Airport &ap = flights_airport();
  snprintf(buf, sizeof(buf), "%s  %s %s  " ICON_CARET_DOWN, pg.arrivals ? ICON_ARRIVAL : ICON_DEPARTURE,
           ap.iata, pg.arrivals ? "Arrivals" : "Departures");
  lv_label_set_text(pg.title, buf);

  const Flight *list = pg.arrivals ? board.arr : board.dep;
  int n = pg.arrivals ? board.n_arr : board.n_dep;
  if (board.updated) {
    char at[16];
    format_time(at, sizeof(at), board.updated, true);
    snprintf(buf, sizeof(buf), "%s  " DOT "  updated %s", ap.name, at);
  } else {
    snprintf(buf, sizeof(buf), "%s  " DOT "  tap title to change", ap.name);
  }
  lv_label_set_text(pg.status, buf);
  lv_label_set_text(pg.empty, !board.updated ? "Looking for flights..."
                              : n ? "" : (pg.arrivals ? "No arrivals right now" : "No departures right now"));

  for (int i = 0; i < MAX_FLIGHTS; i++) {
    if (i >= n) {
      lv_label_set_text(pg.name[i], "");
      lv_label_set_text(pg.time[i], "");
      lv_label_set_text(pg.info[i], "");
      continue;
    }
    const Flight &f = list[i];
    lv_label_set_text(pg.name[i], f.display);
    lv_obj_set_style_text_color(pg.time[i], (!pg.arrivals || f.landed) ? COL_GOOD : COL_ACCENT, 0);
    if (f.landed)          snprintf(buf, sizeof(buf), "Landed");
    else if (f.on_ground)  snprintf(buf, sizeof(buf), "Taxi");
    else if (f.when)       format_time(buf, sizeof(buf), f.when, false);
    else                   buf[0] = 0;
    lv_label_set_text(pg.time[i], buf);

    char place[32];
    const char *dir_word = pg.arrivals ? "From" : "To";
    if (f.other_city[0])  snprintf(place, sizeof(place), "%s %s", dir_word, f.other_city);
    else if (f.other[0])  snprintf(place, sizeof(place), "%s %s", dir_word, f.other);
    else                  snprintf(place, sizeof(place), "%s", f.type[0] ? f.type : "Private");
    char alt[16];
    if (f.on_ground)          snprintf(alt, sizeof(alt), "on ground");
    else if (f.alt_ft >= 1000) snprintf(alt, sizeof(alt), "%.1fk ft", f.alt_ft / 1000.0f);
    else                      snprintf(alt, sizeof(alt), "%d ft", f.alt_ft);
    if (f.landed) {
      char at[16];
      format_time(at, sizeof(at), f.when, false);
      snprintf(buf, sizeof(buf), "%s " DOT " landed %s", place, at);
    } else {
      snprintf(buf, sizeof(buf), "%s " DOT " %.0f nm " DOT " %s", place, f.dist_nm, alt);
    }
    lv_label_set_text(pg.info[i], buf);
  }
}

#ifdef DESKDASH_PREVIEW
void flights_preview_set(const FlightBoard &b) { board = b; version++; }
void flights_preview_open_picker() { open_picker(nullptr); }
#endif
