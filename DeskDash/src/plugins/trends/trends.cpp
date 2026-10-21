// Trending on X: top topics for TRENDS_LOCATION, read from trends24.in (free, unofficial).
#include "../../core/deskdash.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

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
static lv_obj_t *lbl_rank[TRENDS_COUNT], *lbl_name[TRENDS_COUNT], *lbl_count[TRENDS_COUNT];
static lv_obj_t *lbl_status;

static void build(lv_obj_t *p) {
  make_label(p, &font_text_24, COL_TEXT, LV_ALIGN_TOP_MID, 0, 22, "Trending on X");
  lbl_status = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, 0, 54, TRENDS_LOCATION_NAME);
  for (int i = 0; i < TRENDS_COUNT; i++) {
    lv_coord_t y = 92 + i * 34;
    lbl_rank[i] = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_RIGHT, -(SCREEN_W - 50), y, "");
    lbl_name[i] = make_label(p, &font_text_18, COL_TEXT, LV_ALIGN_TOP_LEFT, 62, y, i == 0 ? "Loading..." : "");
    lv_label_set_long_mode(lbl_name[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_name[i], 178);
    lbl_count[i] = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_RIGHT, -26, y, "");
  }
}

static void format_count(char *out, size_t n, long c) {
  if (c < 0)             out[0] = 0;
  else if (c >= 1000000) snprintf(out, n, "%.1fM", c / 1000000.0);
  else if (c >= 10000)   snprintf(out, n, "%ldK", c / 1000);
  else if (c >= 1000)    snprintf(out, n, "%.1fK", c / 1000.0);
  else                   snprintf(out, n, "%ld", c);
}

static void show() {
  char buf[64];
  for (int i = 0; i < TRENDS_COUNT; i++) {
    bool has = i < trends.n;
    snprintf(buf, sizeof(buf), "%d", i + 1);
    lv_label_set_text(lbl_rank[i], has ? buf : "");
    lv_label_set_text(lbl_name[i], has ? trends.items[i].name : "");
    format_count(buf, sizeof(buf), has ? trends.items[i].count : -1);
    lv_label_set_text(lbl_count[i], buf);
  }
  char at[16];
  format_time(at, sizeof(at), trends.updated, true);
  snprintf(buf, sizeof(buf), "%s  " DOT "  %s", TRENDS_LOCATION_NAME, at);
  lv_label_set_text(lbl_status, buf);
}

// Decodes common HTML entities and drops characters the fonts can't draw.
static void clean_name(const String &in, char *out, size_t n) {
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
  while (o > 0 && out[o - 1] == ' ') o--;
  out[o] = 0;
}

// The page is ~250 KB, so it is parsed as a stream: the first <ol class=trend-card__list>
// is the latest hour; each item is a trend-link name plus a data-count.
static bool fetch() {
  String url = String("https://trends24.in/") + TRENDS_LOCATION + "/";
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("Mozilla/5.0 (DeskDash; ESP32-C5)");
  if (!http.begin(client, url)) return false;
  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("[trends] HTTP %d (%s)\n", status, http.errorToString(status).c_str());
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
      clean_name(name, t.items[t.n].name, sizeof(t.items[t.n].name));
      if (t.items[t.n].name[0]) {
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
    show();
    ui_unlock();
  }
  Serial.printf("[trends] %d trends, #1: %s\n", t.n, t.items[0].name);
  return true;
}

static void poll() {
  static uint32_t last = 0;
  static bool ok_once = false;
  if (!dd_wifi_up()) return;
  uint32_t interval = (ok_once ? TRENDS_REFRESH_MINUTES * 60UL : 60UL) * 1000UL;
  if (last == 0 || millis() - last > interval) {
    last = millis();
    if (fetch()) ok_once = true;
  }
}

extern const DeskDashPlugin plugin_trends = {"Trending on X", build, nullptr, poll};

#ifdef DESKDASH_PREVIEW
void trends_preview_set(const char **names, const long *counts, int n) {
  trends.n = n;
  for (int i = 0; i < n; i++) { snprintf(trends.items[i].name, sizeof(trends.items[i].name), "%s", names[i]); trends.items[i].count = counts[i]; }
  trends.updated = time(nullptr) - 600;
  show();
}
#endif
