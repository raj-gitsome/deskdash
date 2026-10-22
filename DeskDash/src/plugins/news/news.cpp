// News page: the latest headlines from RSS and Atom feeds. Tap a headline to see its summary
// and a QR code that opens the full article on your phone.
//
// A set of popular news feeds is built in and switched on. Tap the page title to manage
// feeds: switch any feed on or off, search for more by name or topic (Feedly's public
// feed search, no key needed), and add or remove the ones you find. Choices are saved on
// the board.
//
// Each feed is read as a stream: only the first few items are parsed and the rest of the
// download is dropped, so even large feeds use little memory.
#include "../../core/deskdash.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp32-hal-psram.h>

// ============================================================================
//  Built-in feeds (switched on until you turn them off). Add your own here.
// ============================================================================
struct DefaultFeed { const char *name, *url; };
static const DefaultFeed DEFAULTS[] = {
  {"BBC News",        "https://feeds.bbci.co.uk/news/rss.xml"},
  {"NPR",             "https://feeds.npr.org/1001/rss.xml"},
  {"New York Times",  "https://rss.nytimes.com/services/xml/rss/nyt/HomePage.xml"},
  {"Fox News",        "https://moxie.foxnews.com/google-publisher/latest.xml"},
  {"The Guardian",    "https://www.theguardian.com/world/rss"},
  {"CNBC",            "https://www.cnbc.com/id/100003114/device/rss/rss.html"},
  {"Ars Technica",    "https://feeds.arstechnica.com/arstechnica/index"},
  {"ESPN",            "https://www.espn.com/espn/rss/news"},
  {"Hacker News",     "https://news.ycombinator.com/rss"},
};
static constexpr int N_DEFAULTS = sizeof(DEFAULTS) / sizeof(DEFAULTS[0]);
static constexpr int MAX_ADDED = 12;
static constexpr int MAX_FEEDS = N_DEFAULTS + MAX_ADDED;
static constexpr int PER_FEED = 6;        // headlines kept per feed
static constexpr int ROWS = 5;            // headlines on the page
static constexpr int PER_SOURCE = 2;      // at most this many on the page from one feed
static constexpr int MAX_RESULTS = 8;

struct Feed {
  char name[32];
  char url[200];
  bool on;
  bool added;            // found by search (can be removed); false = built in
};

struct Headline {
  char title[160];
  char link[224];
  char summary[240];
  time_t when;
};

struct FeedNews {
  char url[200];
  int n;
  Headline h[PER_FEED];
  time_t fetched;        // wall clock of the last successful read
  uint32_t tried_ms;     // millis() of the last attempt
  bool tried, ok;
};

struct Result {
  char name[32];
  char url[200];
};

// Shared between the UI thread and poll(); poll() only touches them under ui_lock().
static Feed feeds[MAX_FEEDS];
static int n_feeds = 0;
static FeedNews *news = nullptr;          // MAX_FEEDS entries in PSRAM, matched to feeds by URL
static uint32_t version = 1;              // bumps when headlines or feed choices change
static bool loaded = false;

static char search_query[48];
static volatile bool search_pending = false;
static Result results[MAX_RESULTS];
static int n_results = 0;
static int search_state = 0;              // 0 none, 1 searching, 2 done, 3 failed
static uint32_t results_version = 0;

// ============================================================================
//  Settings: "rss_off" = one '0'/'1' per built-in feed, "rss_add" = "name\turl\n" lines
// ============================================================================
static void save_settings() {
  char off[N_DEFAULTS + 1];
  String added;
  for (int i = 0; i < n_feeds; i++) {
    if (!feeds[i].added) off[i] = feeds[i].on ? '1' : '0';
    else {
      added += feeds[i].on ? "+" : "-";
      added += feeds[i].name;
      added += "\t";
      added += feeds[i].url;
      added += "\n";
    }
  }
  off[N_DEFAULTS] = 0;
  dd_setting_set("rss_off", off);
  dd_setting_set("rss_add", added.c_str());
}

static void load_settings() {
  if (loaded) return;
  loaded = true;
  if (!news) news = (FeedNews *)ps_calloc(MAX_FEEDS, sizeof(FeedNews));
  String off = dd_setting_get("rss_off", "");
  n_feeds = 0;
  for (int i = 0; i < N_DEFAULTS; i++) {
    Feed &f = feeds[n_feeds++];
    snprintf(f.name, sizeof(f.name), "%s", DEFAULTS[i].name);
    snprintf(f.url, sizeof(f.url), "%s", DEFAULTS[i].url);
    f.on = (int)off.length() > i ? off[i] != '0' : true;
    f.added = false;
  }
  String added = dd_setting_get("rss_add", "");
  int start = 0;
  while (start < (int)added.length() && n_feeds < MAX_FEEDS) {
    int nl = added.indexOf('\n', start);
    if (nl < 0) nl = added.length();
    String line = added.substring(start, nl);
    start = nl + 1;
    int tab = line.indexOf('\t');
    if (line.length() < 3 || tab < 2) continue;
    Feed &f = feeds[n_feeds++];
    f.on = line[0] != '-';
    snprintf(f.name, sizeof(f.name), "%s", line.substring(1, tab).c_str());
    snprintf(f.url, sizeof(f.url), "%s", line.substring(tab + 1).c_str());
    f.added = true;
  }
}

static FeedNews *find_news(const char *url) {
  if (!news) return nullptr;
  for (int i = 0; i < MAX_FEEDS; i++)
    if (news[i].url[0] && strcmp(news[i].url, url) == 0) return &news[i];
  return nullptr;
}

static bool feed_listed(const char *url) {
  for (int i = 0; i < n_feeds; i++)
    if (strcmp(feeds[i].url, url) == 0) return true;
  return false;
}

// A slot for a feed's headlines: an empty one, else one whose feed was removed
static FeedNews *news_slot() {
  for (int i = 0; i < MAX_FEEDS; i++) if (!news[i].url[0]) return &news[i];
  for (int i = 0; i < MAX_FEEDS; i++) if (!feed_listed(news[i].url)) return &news[i];
  return &news[0];
}

// ============================================================================
//  Text clean-up: entities, UTF-8 punctuation and accents down to what the fonts draw
// ============================================================================
static const char LATIN1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

static void emit_cp(uint32_t cp, char *out, int &o, int n) {
  const char *s = nullptr;
  char one[2] = {0, 0};
  if (cp == 0x2018 || cp == 0x2019 || cp == 0x2032 || cp == 0x02BC) s = "'";
  else if (cp == 0x201C || cp == 0x201D || cp == 0x2033) s = "\"";
  else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212 || cp == 0x2010) s = "-";
  else if (cp == 0x2026) s = "...";
  else if (cp == 0xA0 || cp == 0x2009 || cp == 0x200A || cp == 0x202F) s = " ";
  else if (cp == 0xB0) s = DEG;
  else if (cp == 0xB7 || cp == 0x2022) s = DOT;
  else if (cp >= 0xC0 && cp <= 0xFF) { one[0] = LATIN1[cp - 0xC0]; s = one; }
  else if (cp >= 0x20 && cp < 0x7F) { one[0] = (char)cp; s = one; }
  else if (cp == '\n' || cp == '\r' || cp == '\t') s = " ";
  if (!s) return;
  for (; *s && o + 1 < n; s++) out[o++] = *s;
}

static uint32_t named_entity(const char *e) {
  static const struct { const char *n; uint32_t cp; } T[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
    {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"rsquo", 0x2019},
    {"lsquo", 0x2018}, {"rdquo", 0x201D}, {"ldquo", 0x201C}, {"deg", 0xB0}, {"middot", 0xB7},
  };
  for (auto &t : T) if (strcmp(e, t.n) == 0) return t.cp;
  return 0;
}

// Decodes one pass of entities and UTF-8 into font-safe text
static void decode_pass(const char *in, char *out, int n) {
  int o = 0;
  for (int i = 0; in[i] && o + 1 < n;) {
    uint8_t c = in[i];
    if (c == '&') {
      int j = i + 1;
      char ent[12];
      int k = 0;
      while (in[j] && in[j] != ';' && k < 10 && in[j] != ' ' && in[j] != '&') ent[k++] = in[j++];
      ent[k] = 0;
      if (in[j] == ';' && k > 0) {
        uint32_t cp = ent[0] == '#' ? (uint32_t)((ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent + 2, nullptr, 16)
                                                                                 : strtoul(ent + 1, nullptr, 10))
                                    : named_entity(ent);
        if (cp) {
          if (cp == '&') out[o++] = '&';   // keep "&amp;lt;" style double escapes for the next pass
          else emit_cp(cp, out, o, n);
          i = j + 1;
          continue;
        }
      }
      out[o++] = '&';
      i++;
    } else if (c >= 0x80) {
      uint32_t cp = 0;
      int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
      cp = len == 4 ? c & 0x07 : len == 3 ? c & 0x0F : c & 0x1F;
      int k = 1;
      for (; k < len && (in[i + k] & 0xC0) == 0x80; k++) cp = (cp << 6) | (in[i + k] & 0x3F);
      if (k == len && len > 1) emit_cp(cp, out, o, n);
      i += k;
    } else {
      emit_cp(c, out, o, n);
      i++;
    }
  }
  out[o] = 0;
}

static void clean_text(const char *in, char *out, int n) {
  static char a[400], b[400];
  decode_pass(in, a, sizeof(a));
  decode_pass(a, b, sizeof(b));      // titles are sometimes escaped twice
  // Drop HTML tags and squeeze spaces
  int o = 0;
  bool in_tag = false, space = true;
  for (int i = 0; b[i] && o + 1 < n; i++) {
    char c = b[i];
    if (c == '<' && (isalpha((uint8_t)b[i + 1]) || b[i + 1] == '/')) { in_tag = true; continue; }
    if (in_tag) { if (c == '>') in_tag = false; continue; }
    if (c == ' ') { if (!space) out[o++] = ' '; space = true; continue; }
    out[o++] = c;
    space = false;
  }
  while (o > 0 && out[o - 1] == ' ') o--;
  out[o] = 0;
}

// ============================================================================
//  Dates: RFC 822 (RSS) and ISO 8601 (Atom) to UTC epoch
// ============================================================================
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int)doe - 719468;
}

static int zone_offset(const char *z) {   // seconds east of UTC
  while (*z == ' ') z++;
  if (*z == '+' || *z == '-') {
    int sign = *z == '-' ? -1 : 1;
    z++;
    int hh = 0, mm = 0;
    if (isdigit((uint8_t)z[0]) && isdigit((uint8_t)z[1])) {
      hh = (z[0] - '0') * 10 + (z[1] - '0');
      z += 2;
      if (*z == ':') z++;
      if (isdigit((uint8_t)z[0]) && isdigit((uint8_t)z[1])) mm = (z[0] - '0') * 10 + (z[1] - '0');
    }
    return sign * (hh * 3600 + mm * 60);
  }
  static const struct { const char *n; int h; } Z[] = {
    {"EST", -5}, {"EDT", -4}, {"CST", -6}, {"CDT", -5}, {"MST", -7}, {"MDT", -6}, {"PST", -8}, {"PDT", -7},
  };
  for (auto &e : Z) if (strncmp(z, e.n, 3) == 0) return e.h * 3600;
  return 0;   // GMT, UT, UTC, Z or unknown
}

static time_t parse_date(const char *s) {
  while (*s == ' ') s++;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  const char *rest = nullptr;
  if (isdigit((uint8_t)s[0]) && s[4] == '-') {                  // 2026-10-22T07:28:24.000-04:00
    if (sscanf(s, "%4d-%2d-%2d%*c%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &sec) < 5) return 0;
    rest = s + 16;
    if (*rest == ':') rest += 3;
    if (*rest == '.') { rest++; while (isdigit((uint8_t)*rest)) rest++; }
  } else {                                                      // Thu, 22 Oct 2026 07:28:24 -0400
    const char *p = strchr(s, ',');
    p = p ? p + 1 : s;
    char mon[4] = {0};
    int used = 0;
    if (sscanf(p, " %d %3s %d %d:%d%n", &d, mon, &y, &h, &mi, &used) < 5) return 0;
    rest = p + used;
    if (*rest == ':') { sec = atoi(rest + 1); rest++; while (isdigit((uint8_t)*rest)) rest++; }
    static const char *M = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *f = strstr(M, mon);
    if (!f) return 0;
    mo = (f - M) / 3 + 1;
    if (y < 100) y += 2000;
  }
  if (y < 1990 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  int64_t t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
  return (time_t)(t - zone_offset(rest));
}

// ============================================================================
//  Streaming RSS/Atom reader
// ============================================================================
struct Reader {
  Client *c = nullptr;
  uint8_t buf[1024];
  int len = 0, pos = 0, back = -1;
  uint32_t deadline = 0;
  size_t total = 0;

  int get() {
    if (back >= 0) { int b = back; back = -1; return b; }
    if (pos >= len) {
      if (total > 400000) return -1;
      for (;;) {
        int a = c->available();
        if (a > 0) {
          len = c->read(buf, a < (int)sizeof(buf) ? a : (int)sizeof(buf));
          if (len <= 0) return -1;
          pos = 0;
          total += len;
          break;
        }
        if (!c->connected() || (int32_t)(millis() - deadline) > 0) return -1;
        delay(2);
      }
    }
    return buf[pos++];
  }
  void unget(int b) { back = b; }
};

static void skip_until(Reader &r, const char *end) {   // e.g. "-->" or "]]>"
  int n = strlen(end), m = 0, c;
  while ((c = r.get()) >= 0) {
    if (c == end[m]) { if (++m == n) return; }
    else m = (c == end[0]) ? 1 : 0;
  }
}

// Skips the rest of a tag's attributes; true if it was self-closing ("/>")
static bool skip_tag(Reader &r, int c) {
  int prev = 0, quote = 0;
  while (c >= 0 && !(c == '>' && !quote)) {
    if (quote) { if (c == quote) quote = 0; }
    else if (c == '"' || c == '\'') quote = c;
    prev = c;
    c = r.get();
  }
  return prev == '/';
}

// Like skip_tag, but keeps the href and rel attributes (Atom <link href="..." rel="..."/>)
static bool read_link_attrs(Reader &r, int c, char *href, int hn, char *rel, int rn) {
  char name[12] = "";
  int nl = 0, prev = 0;
  href[0] = rel[0] = 0;
  while (c >= 0 && c != '>') {
    if (c == '"' || c == '\'') {
      int q = c, o = 0;
      char *dst = strcmp(name, "href") == 0 ? href : strcmp(name, "rel") == 0 ? rel : nullptr;
      int dn = dst == href ? hn : rn;
      while ((c = r.get()) >= 0 && c != q)
        if (dst && o + 1 < dn) dst[o++] = (char)c;
      if (dst) dst[o] = 0;
      nl = 0;
      name[0] = 0;
    } else if (isalnum(c) || c == ':' || c == '_' || c == '-') {
      if (nl + 1 < (int)sizeof(name)) { name[nl++] = (char)c; name[nl] = 0; }
    } else if (c != '=') {
      nl = 0;
      if (c != ' ' && c != '\n' && c != '\t' && c != '\r') name[0] = 0;
    }
    prev = c;
    c = r.get();
  }
  return prev == '/';
}

static void set_link(char *dst, const char *src) {
  while (*src == ' ' || *src == '\n' || *src == '\r' || *src == '\t') src++;
  if (strncmp(src, "http", 4) != 0) return;
  int o = 0;
  for (; src[o] && o < 223 && src[o] != ' ' && src[o] != '\n' && src[o] != '\r'; o++) dst[o] = src[o];
  dst[o] = 0;
  // &amp; in a URL is just &
  for (char *a = strstr(dst, "&amp;"); a; a = strstr(a + 1, "&amp;")) memmove(a + 1, a + 5, strlen(a + 5) + 1);
}

// Text of an element up to its closing tag; CDATA kept, inline tags dropped
static void read_text(Reader &r, char *out, int n) {
  int o = 0, c;
  while ((c = r.get()) >= 0) {
    if (c == '<') {
      int d = r.get();
      if (d == '!') {
        char head[8] = {0};
        for (int k = 0; k < 7; k++) head[k] = (char)r.get();
        if (strcmp(head, "[CDATA[") == 0) {
          int w1 = 0, w2 = 0;
          while ((c = r.get()) >= 0) {
            if (c == '>' && w1 == ']' && w2 == ']') { o = o >= 2 ? o - 2 : 0; break; }
            if (o + 1 < n) out[o++] = (char)c;
            w2 = w1;
            w1 = c;
          }
        } else {
          skip_until(r, ">");
        }
        continue;
      }
      bool closing = d == '/';
      skip_tag(r, d);
      if (closing) break;
      continue;
    }
    if (o + 1 < n) out[o++] = (char)c;
  }
  out[o] = 0;
}

// Reads a tag name after '<'; returns the character that ended it
static int read_name(Reader &r, char *name, int n) {
  int o = 0, c = r.get();
  if (c == '/') { name[o++] = '/'; c = r.get(); }
  while (c >= 0 && c != '>' && c != '/' && c != ' ' && c != '\n' && c != '\r' && c != '\t') {
    if (o + 1 < n) name[o++] = (char)c;
    if (o == 3 && strncmp(name, "!--", 3) == 0) break;
    if (o == 8 && strncmp(name, "![CDATA[", 8) == 0) break;
    c = r.get();
  }
  name[o] = 0;
  return c;
}

static void parse_feed(Reader &r, FeedNews &out) {
  static char raw[1200];
  bool in_item = false;
  Headline cur;
  int c;
  while ((c = r.get()) >= 0) {
    if (c != '<') continue;
    char name[24];
    int end = read_name(r, name, sizeof(name));
    if (strncmp(name, "!--", 3) == 0) { skip_until(r, "-->"); continue; }
    if (strncmp(name, "![CDATA[", 8) == 0) { skip_until(r, "]]>"); continue; }
    if (name[0] == '!' || name[0] == '?') { skip_tag(r, end); continue; }
    bool closing = name[0] == '/';
    const char *nm = closing ? name + 1 : name;
    bool self_closing;
    char href[224], rel[16];
    href[0] = rel[0] = 0;
    if (in_item && !closing && strcmp(nm, "link") == 0)
      self_closing = read_link_attrs(r, end, href, sizeof(href), rel, sizeof(rel));
    else
      self_closing = skip_tag(r, end);
    bool item_tag = strcmp(nm, "item") == 0 || strcmp(nm, "entry") == 0;
    if (item_tag && !closing && !self_closing) {
      in_item = true;
      memset(&cur, 0, sizeof(cur));
      continue;
    }
    if (item_tag && closing) {
      if (in_item && cur.title[0]) {
        out.h[out.n++] = cur;
        if (out.n >= PER_FEED) return;
      }
      in_item = false;
      continue;
    }
    if (!in_item || closing) continue;
    if (strcmp(nm, "link") == 0) {
      if (href[0]) {                                     // Atom
        if (!cur.link[0] && (!rel[0] || strcmp(rel, "alternate") == 0)) set_link(cur.link, href);
      }
      if (!self_closing) {                               // RSS: <link>url</link>
        read_text(r, raw, sizeof(raw));
        if (!cur.link[0]) set_link(cur.link, raw);
      }
      continue;
    }
    if (self_closing) continue;
    if (strcmp(nm, "title") == 0) {
      read_text(r, raw, sizeof(raw));
      clean_text(raw, cur.title, sizeof(cur.title));
    } else if (strcmp(nm, "feedburner:origLink") == 0) {
      read_text(r, raw, sizeof(raw));
      set_link(cur.link, raw);
    } else if (strcmp(nm, "guid") == 0 || strcmp(nm, "id") == 0) {
      read_text(r, raw, sizeof(raw));
      if (!cur.link[0]) set_link(cur.link, raw);
    } else if (!cur.summary[0] && (strcmp(nm, "description") == 0 || strcmp(nm, "summary") == 0)) {
      read_text(r, raw, sizeof(raw));
      clean_text(raw, cur.summary, sizeof(cur.summary));
      if (strlen(cur.summary) < 25) cur.summary[0] = 0;   // e.g. just "Comments"
    } else if (strcmp(nm, "pubDate") == 0 || strcmp(nm, "published") == 0 || strcmp(nm, "dc:date") == 0 ||
               (strcmp(nm, "updated") == 0 && !cur.when)) {
      read_text(r, raw, sizeof(raw));
      time_t t = parse_date(raw);
      if (t) cur.when = t;
    }
  }
}

// ============================================================================
//  HTTP
// ============================================================================
// Opens url (following redirects, which may switch between http and https). On success the
// response body is ready to read; the caller ends the request either way.
static bool open_url(HTTPClient &http, WiFiClient &plain, WiFiClientSecure &tls, const char *start_url) {
  static char url[256];
  snprintf(url, sizeof(url), "%s", start_url);
  bool http10 = true;
  for (int hop = 0; hop < 5; hop++) {
    bool https = strncmp(url, "https://", 8) == 0;
    http.setTimeout(12000);
    http.useHTTP10(http10);             // 1.0 avoids chunked encoding, so the stream is plain XML
    http.setUserAgent("Mozilla/5.0 (DeskDash; ESP32-C5) RSS reader");
    const char *keys[] = {"Location"};
    http.collectHeaders(keys, 1);
    bool ok = https ? http.begin(tls, url) : http.begin(plain, url);
    if (!ok) return false;
    int status = http.GET();
    if (status == 200) return true;
    if (status == 426 && http10) { http.end(); http10 = false; hop--; continue; }
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
      String loc = http.header("Location");
      http.end();
      if (loc.length() == 0) return false;
      if (loc[0] == '/') {                 // relative: keep scheme and host
        char *host_end = strchr(url + (https ? 8 : 7), '/');
        if (host_end) *host_end = 0;
        strncat(url, loc.c_str(), sizeof(url) - strlen(url) - 1);
      } else {
        snprintf(url, sizeof(url), "%s", loc.c_str());
      }
      continue;
    }
    Serial.printf("[news] %s -> HTTP %d\n", url, status);
    return false;
  }
  return false;
}

static bool fetch_feed(const char *url, FeedNews &out) {
  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure tls;
  tls.setInsecure();                    // public data; skips certificate checks
  if (!open_url(http, plain, tls, url)) { http.end(); return false; }
  static Reader r;
  r = Reader();
  r.c = http.getStreamPtr();
  r.deadline = millis() + 15000;
  if (r.c) parse_feed(r, out);
  http.end();
  // Undated items keep their order, a minute apart; dates in the future count as now
  time_t now = time(nullptr);
  for (int i = 0; i < out.n; i++) {
    if (!out.h[i].when) out.h[i].when = now - i * 60;
    if (out.h[i].when > now) out.h[i].when = now;
  }
  return out.n > 0;
}

static void url_encode(const char *in, char *out, int n) {
  int o = 0;
  for (; *in && o + 4 < n; in++) {
    uint8_t c = *in;
    if (isalnum(c) || c == '-' || c == '_' || c == '.') out[o++] = c;
    else if (c == ' ') out[o++] = '+';
    else o += snprintf(out + o, n - o, "%%%02X", c);
  }
  out[o] = 0;
}

static void do_search() {
  char q[48], enc[150], url[220];
  if (!ui_lock()) return;
  snprintf(q, sizeof(q), "%s", search_query);
  ui_unlock();
  url_encode(q, enc, sizeof(enc));
  snprintf(url, sizeof(url), "https://cloud.feedly.com/v3/search/feeds?count=%d&locale=en&query=%s", MAX_RESULTS, enc);

  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.setTimeout(12000);
  http.setUserAgent("DeskDash/1.0 (ESP32-C5 desk display)");
  static Result found[MAX_RESULTS];
  int n = 0;
  bool ok = false;
  if (http.begin(tls, url)) {
    int status = http.GET();
    if (status == 200) {
      String body = http.getString();
      JsonDocument filter;
      filter["results"][0]["feedId"] = true;
      filter["results"][0]["title"] = true;
      JsonDocument doc;
      if (!deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
        ok = true;
        for (JsonObject r : doc["results"].as<JsonArray>()) {
          const char *id = r["feedId"] | "";
          if (strncmp(id, "feed/http", 9) != 0 || n >= MAX_RESULTS) continue;
          snprintf(found[n].url, sizeof(found[n].url), "%s", id + 5);
          clean_text(r["title"] | id + 5, found[n].name, sizeof(found[n].name));
          if (!found[n].name[0]) snprintf(found[n].name, sizeof(found[n].name), "%s", id + 5);
          n++;
        }
      }
    } else {
      Serial.printf("[news] search -> HTTP %d\n", status);
    }
    http.end();
  }
  if (ui_lock()) {
    memcpy(results, found, sizeof(results));
    n_results = n;
    search_state = ok ? 2 : 3;
    results_version++;
    ui_unlock();
  }
  Serial.printf("[news] search \"%s\": %d feeds\n", q, n);
}

// ============================================================================
//  Polling: one feed per call, each feed every RSS_REFRESH_MINUTES
// ============================================================================
static void poll() {
  if (!loaded || !news || !dd_wifi_up()) return;
  if (search_pending) {
    search_pending = false;
    do_search();
    return;
  }
  static uint32_t last_fetch = 0;
  if (last_fetch && millis() - last_fetch < 2000) return;   // let other pages' requests through

  char url[200] = "", name[32] = "";
  if (!ui_lock()) return;
  for (int i = 0; i < n_feeds; i++) {
    if (!feeds[i].on) continue;
    FeedNews *fn = find_news(feeds[i].url);
    uint32_t wait = (fn && fn->ok) ? RSS_REFRESH_MINUTES * 60000UL : 5 * 60000UL;
    if (!fn || !fn->tried || millis() - fn->tried_ms > wait) {
      strcpy(url, feeds[i].url);
      strcpy(name, feeds[i].name);
      break;
    }
  }
  ui_unlock();
  if (!url[0]) return;

  last_fetch = millis();
  static FeedNews *gotp = (FeedNews *)ps_malloc(sizeof(FeedNews));
  if (!gotp) return;
  FeedNews &got = *gotp;
  memset(&got, 0, sizeof(got));
  strcpy(got.url, url);
  bool ok = fetch_feed(url, got);
  if (ui_lock()) {
    FeedNews *fn = find_news(url);
    if (!fn) { fn = news_slot(); memset(fn, 0, sizeof(*fn)); strcpy(fn->url, url); }
    if (ok) { *fn = got; fn->fetched = time(nullptr); }
    fn->tried = true;
    fn->ok = ok;
    fn->tried_ms = millis();
    version++;
    ui_unlock();
  }
  Serial.printf("[news] %s: %d headlines%s\n", name, got.n, ok ? "" : " (failed)");
}

// ============================================================================
//  Feed manager (overlay; UI thread)
// ============================================================================
static lv_obj_t *mgr, *mgr_list, *mgr_ta, *mgr_kb, *mgr_done;
static uint32_t mgr_results_shown = 0;

static void rebuild_list(void * = nullptr);

static lv_obj_t *make_text_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_color_t fg,
                                  lv_coord_t w, lv_coord_t h, const lv_font_t *font) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  make_label(b, font, fg, LV_ALIGN_CENTER, 0, 0, text);
  return b;
}

static void toggle_cb(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= n_feeds) return;
  feeds[i].on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  save_settings();
  version++;
}

static void remove_cb(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= n_feeds || !feeds[i].added) return;
  for (int k = i; k < n_feeds - 1; k++) feeds[k] = feeds[k + 1];
  n_feeds--;
  save_settings();
  version++;
  lv_async_call(rebuild_list, nullptr);      // this button is deleted by the rebuild
}

static void add_cb(lv_event_t *e) {
  int j = (int)(intptr_t)lv_event_get_user_data(e);
  if (j < 0 || j >= n_results || feed_listed(results[j].url) || n_feeds >= MAX_FEEDS) return;
  Feed &f = feeds[n_feeds++];
  snprintf(f.name, sizeof(f.name), "%s", results[j].name);
  snprintf(f.url, sizeof(f.url), "%s", results[j].url);
  f.on = true;
  f.added = true;
  save_settings();
  version++;
  lv_async_call(rebuild_list, nullptr);
}

static lv_obj_t *list_row(lv_obj_t *parent) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, 288, 50);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
  return row;
}

static lv_obj_t *row_name(lv_obj_t *row, const char *text, lv_coord_t w, lv_color_t col) {
  lv_obj_t *l = make_label(row, &font_text_18, col, LV_ALIGN_LEFT_MID, 4, 0, text);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_size(l, w, 22);           // one line
  return l;
}

static void section(lv_obj_t *parent, const char *text) {
  lv_obj_t *row = list_row(parent);
  lv_obj_set_height(row, 34);
  make_label(row, &font_text_18, COL_MUTED, LV_ALIGN_BOTTOM_LEFT, 4, -4, text);
}

static void rebuild_list(void *) {
  if (!mgr_list) return;
  lv_obj_clean(mgr_list);
  if (search_state != 0) {
    section(mgr_list, "Search results");
    if (search_state == 1 || search_state == 3 || n_results == 0) {
      lv_obj_t *row = list_row(mgr_list);
      row_name(row, search_state == 1 ? "Searching..." : search_state == 3 ? "Search failed, try again"
                                                                           : "No feeds found", 280, COL_MUTED);
    }
    if (search_state == 2) {
      for (int j = 0; j < n_results; j++) {
        lv_obj_t *row = list_row(mgr_list);
        row_name(row, results[j].name, 196, COL_TEXT);
        if (feed_listed(results[j].url)) {
          make_label(row, &font_text_18, COL_GOOD, LV_ALIGN_RIGHT_MID, -6, 0, "Added");
        } else if (n_feeds < MAX_FEEDS) {
          lv_obj_t *b = make_text_button(row, "Add", COL_ACCENT, COL_BG, 72, 38, &font_text_18);
          lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
          lv_obj_add_event_cb(b, add_cb, LV_EVENT_CLICKED, (void *)(intptr_t)j);
        } else {
          make_label(row, &font_text_18, COL_MUTED, LV_ALIGN_RIGHT_MID, -6, 0, "List full");
        }
      }
    }
  }
  section(mgr_list, "Your feeds");
  for (int i = 0; i < n_feeds; i++) {
    lv_obj_t *row = list_row(mgr_list);
    row_name(row, feeds[i].name, feeds[i].added ? 168 : 214, COL_TEXT);
    if (feeds[i].added) {
      lv_obj_t *x = make_text_button(row, "x", COL_FAINT, COL_TEXT, 38, 38, &font_text_18);
      lv_obj_align(x, LV_ALIGN_RIGHT_MID, -66, 0);
      lv_obj_add_event_cb(x, remove_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 56, 30);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A4456), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (feeds[i].on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, toggle_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
  }
  mgr_results_shown = results_version;
}

static void show_keyboard(bool show) {
  if (!mgr) return;
  if (show) {
    lv_obj_clear_flag(mgr_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mgr_done, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(mgr_list, SCREEN_H - 112 - 212);
    lv_obj_add_state(mgr_ta, LV_STATE_FOCUSED);
  } else {
    lv_obj_add_flag(mgr_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(mgr_done, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(mgr_list, SCREEN_H - 112 - 86);
    lv_obj_clear_state(mgr_ta, LV_STATE_FOCUSED);
  }
}

static void start_search() {
  const char *t = lv_textarea_get_text(mgr_ta);
  while (*t == ' ') t++;
  if (strlen(t) < 2) return;
  snprintf(search_query, sizeof(search_query), "%s", t);
  search_state = 1;
  n_results = 0;
  search_pending = true;
  show_keyboard(false);
  rebuild_list();
}

static const char *KB_MAP[] = {
  "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
  "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
  "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
  "z", "x", "c", "v", "b", "n", "m", "Del", "\n",
  "Hide", "space", "Search", "",
};

static void kb_cb(lv_event_t *e) {
  lv_obj_t *kb = lv_event_get_target(e);
  uint16_t id = lv_btnmatrix_get_selected_btn(kb);
  if (id == LV_BTNMATRIX_BTN_NONE) return;
  const char *t = lv_btnmatrix_get_btn_text(kb, id);
  if (!t) return;
  if (strcmp(t, "Del") == 0) lv_textarea_del_char(mgr_ta);
  else if (strcmp(t, "space") == 0) lv_textarea_add_char(mgr_ta, ' ');
  else if (strcmp(t, "Hide") == 0) show_keyboard(false);
  else if (strcmp(t, "Search") == 0) start_search();
  else lv_textarea_add_text(mgr_ta, t);
}

static void ta_cb(lv_event_t *e) { show_keyboard(true); }

static void close_mgr(lv_event_t *e) {
  if (mgr) lv_obj_del(mgr);
  mgr = mgr_list = mgr_ta = mgr_kb = mgr_done = nullptr;
  version++;
}

static void open_mgr(lv_event_t *e) {
  if (mgr) return;
  load_settings();
  mgr = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(mgr);
  lv_obj_set_size(mgr, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(mgr, COL_BG, 0);
  lv_obj_set_style_bg_opa(mgr, LV_OPA_COVER, 0);
  lv_obj_add_flag(mgr, LV_OBJ_FLAG_CLICKABLE);       // swallow touches meant for the page below
  lv_obj_clear_flag(mgr, LV_OBJ_FLAG_SCROLLABLE);

  make_label(mgr, &font_text_24, COL_TEXT, LV_ALIGN_TOP_MID, 0, 18, "News feeds");

  mgr_ta = lv_textarea_create(mgr);
  lv_textarea_set_one_line(mgr_ta, true);
  lv_textarea_set_max_length(mgr_ta, 40);
  lv_textarea_set_placeholder_text(mgr_ta, "Search for feeds");
  lv_obj_set_size(mgr_ta, 288, 46);
  lv_obj_align(mgr_ta, LV_ALIGN_TOP_MID, 0, 56);
  lv_obj_set_style_text_font(mgr_ta, &font_text_18, 0);
  lv_obj_set_style_text_color(mgr_ta, COL_TEXT, 0);
  lv_obj_set_style_text_color(mgr_ta, COL_MUTED, LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_bg_color(mgr_ta, COL_PANEL, 0);
  lv_obj_set_style_bg_opa(mgr_ta, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(mgr_ta, COL_FAINT, 0);
  lv_obj_set_style_border_color(mgr_ta, COL_ACCENT, LV_STATE_FOCUSED);
  lv_obj_set_style_radius(mgr_ta, 12, 0);
  lv_obj_set_style_pad_hor(mgr_ta, 14, 0);
  lv_obj_set_style_pad_ver(mgr_ta, 12, 0);
  lv_obj_set_style_bg_color(mgr_ta, COL_ACCENT, LV_PART_CURSOR);
  lv_obj_set_scrollbar_mode(mgr_ta, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(mgr_ta, ta_cb, LV_EVENT_CLICKED, NULL);
  if (search_query[0]) lv_textarea_set_text(mgr_ta, search_query);

  mgr_list = lv_obj_create(mgr);
  lv_obj_remove_style_all(mgr_list);
  lv_obj_set_size(mgr_list, 296, SCREEN_H - 112 - 86);
  lv_obj_align(mgr_list, LV_ALIGN_TOP_MID, 0, 112);
  lv_obj_set_flex_flow(mgr_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(mgr_list, 2, 0);
  lv_obj_set_scroll_dir(mgr_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(mgr_list, LV_SCROLLBAR_MODE_ACTIVE);

  mgr_done = make_text_button(mgr, "Done", COL_ACCENT, COL_BG, 150, 52, &font_text_24);
  lv_obj_align(mgr_done, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_add_event_cb(mgr_done, close_mgr, LV_EVENT_CLICKED, NULL);

  mgr_kb = lv_btnmatrix_create(mgr);
  lv_btnmatrix_set_map(mgr_kb, KB_MAP);
  lv_btnmatrix_set_btn_width(mgr_kb, 36, 2);         // Del
  lv_btnmatrix_set_btn_width(mgr_kb, 37, 3);         // Hide
  lv_btnmatrix_set_btn_width(mgr_kb, 38, 4);         // space
  lv_btnmatrix_set_btn_width(mgr_kb, 39, 3);         // Search
  lv_obj_set_size(mgr_kb, SCREEN_W, 208);
  lv_obj_align(mgr_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_text_font(mgr_kb, &font_text_18, 0);
  lv_obj_set_style_bg_color(mgr_kb, COL_PANEL, 0);
  lv_obj_set_style_bg_opa(mgr_kb, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(mgr_kb, 0, 0);
  lv_obj_set_style_radius(mgr_kb, 0, 0);
  lv_obj_set_style_pad_all(mgr_kb, 4, 0);
  lv_obj_set_style_pad_gap(mgr_kb, 4, 0);
  lv_obj_set_style_bg_color(mgr_kb, lv_color_hex(0x2A3240), LV_PART_ITEMS);
  lv_obj_set_style_text_color(mgr_kb, COL_TEXT, LV_PART_ITEMS);
  lv_obj_set_style_radius(mgr_kb, 6, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(mgr_kb, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(mgr_kb, COL_ACCENT, LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_add_event_cb(mgr_kb, kb_cb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_flag(mgr_kb, LV_OBJ_FLAG_HIDDEN);

  rebuild_list();
}

// ============================================================================
//  Page
// ============================================================================
static lv_obj_t *lbl_title, *lbl_status, *lbl_empty;
static lv_obj_t *lbl_head[ROWS], *lbl_meta[ROWS], *row_hit[ROWS];
static const Headline *row_item[ROWS];
static int row_feed[ROWS];
static uint32_t shown = 0;
static uint32_t shown_ms = 0;

static void format_age(char *out, size_t n, time_t when);

// ---- Article card: headline, source, summary and a QR code for the link ----------------
static lv_obj_t *article = nullptr;
static lv_timer_t *article_timer = nullptr;

static void close_article(lv_event_t *e) {
  if (article) lv_obj_del_async(article);
  article = nullptr;
  if (article_timer) { lv_timer_del(article_timer); article_timer = nullptr; }
}

// Pages stop cycling while a card is open, so close it by itself after 2 minutes
static void article_timeout(lv_timer_t *t) {
  article_timer = nullptr;           // a one-shot timer deletes itself
  close_article(nullptr);
}

static void open_article(lv_event_t *e) {
  int row = (int)(intptr_t)lv_event_get_user_data(e);
  if (article || row < 0 || row >= ROWS || !row_item[row]) return;
  const Headline &h = *row_item[row];
  article = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(article);
  lv_obj_set_size(article, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(article, COL_BG, 0);
  lv_obj_set_style_bg_opa(article, LV_OPA_COVER, 0);
  lv_obj_add_flag(article, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(article, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(article, close_article, LV_EVENT_CLICKED, NULL);   // tap anywhere to close
  article_timer = lv_timer_create(article_timeout, 120000, NULL);
  lv_timer_set_repeat_count(article_timer, 1);

  char buf[96], age[16];
  format_age(age, sizeof(age), h.when);
  snprintf(buf, sizeof(buf), "%s  " DOT "  %s", feeds[row_feed[row]].name, age);
  make_label(article, &font_text_18, COL_ACCENT, LV_ALIGN_TOP_LEFT, 18, 18, buf);

  lv_obj_t *t = make_label(article, &font_text_24, COL_TEXT, LV_ALIGN_TOP_LEFT, 18, 46, h.title);
  lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_line_space(t, 2, 0);
  lv_obj_set_width(t, 284);
  lv_obj_update_layout(t);
  if (lv_obj_get_height(t) > 120) lv_obj_set_height(t, 120);       // at most 4 lines
  lv_obj_update_layout(t);
  lv_coord_t y = 46 + lv_obj_get_height(t) + 10;

  bool has_link = h.link[0] != 0;
  lv_coord_t qr_top = SCREEN_H - 214;
  if (h.summary[0]) {
    lv_obj_t *sm = make_label(article, &font_text_18, COL_MUTED, LV_ALIGN_TOP_LEFT, 18, y, h.summary);
    lv_label_set_long_mode(sm, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_line_space(sm, 2, 0);
    lv_obj_set_width(sm, 284);
    lv_obj_update_layout(sm);
    lv_coord_t room = (has_link ? qr_top - 8 : SCREEN_H - 40) - y;
    if (lv_obj_get_height(sm) > room) lv_obj_set_height(sm, room > 22 ? room - room % 22 : 22);
  }

  if (has_link) {
    lv_obj_t *qr = lv_qrcode_create(article, 156, lv_color_hex(0x000000), lv_color_hex(0xFFFFFF));
    lv_qrcode_update(qr, h.link, strlen(h.link));
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, qr_top);
    lv_obj_set_style_border_color(qr, lv_color_hex(0xFFFFFF), 0);   // quiet zone so phones can read it
    lv_obj_set_style_border_width(qr, 8, 0);
    lv_obj_clear_flag(qr, LV_OBJ_FLAG_CLICKABLE);
    make_label(article, &font_text_18, COL_MUTED, LV_ALIGN_BOTTOM_MID, 0, -14, "Scan to read  " DOT "  tap to close");
  } else {
    make_label(article, &font_text_18, COL_MUTED, LV_ALIGN_BOTTOM_MID, 0, -14, "No link in this feed  " DOT "  tap to close");
  }
}

static void build(lv_obj_t *p) {
  load_settings();
  lbl_title = make_label(p, &font_text_24, COL_TEXT, LV_ALIGN_TOP_MID, 0, 22, "News  " ICON_CARET_DOWN);
  lv_obj_add_flag(lbl_title, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(lbl_title, 16);
  lv_obj_add_event_cb(lbl_title, open_mgr, LV_EVENT_CLICKED, NULL);
  lbl_status = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, 0, 54, "tap title to choose feeds");
  lbl_empty = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_MID, 0, 200, "Loading headlines...");
  for (int i = 0; i < ROWS; i++) {
    lv_coord_t y = 86 + i * 70;
    lbl_head[i] = make_label(p, &font_text_18, COL_TEXT, LV_ALIGN_TOP_LEFT, 18, y, "");
    lv_label_set_long_mode(lbl_head[i], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_line_space(lbl_head[i], 2, 0);
    lv_obj_set_size(lbl_head[i], 284, 42);
    lbl_meta[i] = make_label(p, &font_text_18, COL_MUTED, LV_ALIGN_TOP_LEFT, 18, y + 43, "");
    lv_label_set_long_mode(lbl_meta[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_meta[i], 284);
    // Invisible tap target over the whole headline; swipes still change pages
    row_hit[i] = lv_obj_create(p);
    lv_obj_remove_style_all(row_hit[i]);
    lv_obj_set_size(row_hit[i], SCREEN_W - 16, 66);
    lv_obj_set_pos(row_hit[i], 8, y - 2);
    lv_obj_add_flag(row_hit[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row_hit[i], open_article, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    row_item[i] = nullptr;
  }
}

static void format_age(char *out, size_t n, time_t when) {
  long s = (long)(time(nullptr) - when);
  if (s < 90)         snprintf(out, n, "just now");
  else if (s < 3600)  snprintf(out, n, "%ldm ago", s / 60);
  else if (s < 86400) snprintf(out, n, "%ldh ago", s / 3600);
  else                snprintf(out, n, "%ldd ago", s / 86400);
}

static void tick() {
  if (mgr && mgr_results_shown != results_version) rebuild_list();
  if (shown == version && millis() - shown_ms < 60000) return;   // ages refresh every minute
  shown = version;
  shown_ms = millis();
  if (!news) return;

  struct Pick { const Headline *h; int feed; };
  static Pick picks[MAX_FEEDS * PER_FEED];
  int np = 0, on = 0;
  time_t updated = 0;
  for (int i = 0; i < n_feeds; i++) {
    if (!feeds[i].on) continue;
    on++;
    FeedNews *fn = find_news(feeds[i].url);
    if (!fn) continue;
    if (fn->fetched > updated) updated = fn->fetched;
    for (int k = 0; k < fn->n; k++) picks[np++] = {&fn->h[k], i};
  }
  for (int i = 1; i < np; i++)            // newest first
    for (int j = i; j > 0 && picks[j].h->when > picks[j - 1].h->when; j--) { Pick t = picks[j]; picks[j] = picks[j - 1]; picks[j - 1] = t; }

  int row = 0;
  int per_feed[MAX_FEEDS] = {0};
  char buf[96], age[16];
  for (int i = 0; i < np && row < ROWS; i++) {
    if (per_feed[picks[i].feed] >= PER_SOURCE) continue;
    per_feed[picks[i].feed]++;
    lv_label_set_text(lbl_head[row], picks[i].h->title);
    format_age(age, sizeof(age), picks[i].h->when);
    snprintf(buf, sizeof(buf), "%s  " DOT "  %s", feeds[picks[i].feed].name, age);
    lv_label_set_text(lbl_meta[row], buf);
    row_item[row] = picks[i].h;
    row_feed[row] = picks[i].feed;
    row++;
  }
  for (; row < ROWS; row++) {
    lv_label_set_text(lbl_head[row], "");
    lv_label_set_text(lbl_meta[row], "");
    row_item[row] = nullptr;
  }

  if (!on) {
    lv_label_set_text(lbl_status, "tap title to choose feeds");
    lv_label_set_text(lbl_empty, "All feeds are off");
  } else {
    if (updated) {
      char at[16];
      format_time(at, sizeof(at), updated, true);
      snprintf(buf, sizeof(buf), "%d feed%s  " DOT "  updated %s", on, on == 1 ? "" : "s", at);
    } else {
      snprintf(buf, sizeof(buf), "%d feed%s", on, on == 1 ? "" : "s");
    }
    lv_label_set_text(lbl_status, buf);
    lv_label_set_text(lbl_empty, np ? "" : "Loading headlines...");
  }
}

extern const DeskDashPlugin plugin_news = {"News", build, tick, poll};

#ifdef DESKDASH_PREVIEW
// Desktop preview: fill headlines, and optionally open the manager with sample results
void news_preview_set(const char **titles, const int *feed_idx, const int *mins, int n) {
  load_settings();
  for (int i = 0; i < n; i++) {
    const char *url = feeds[feed_idx[i]].url;
    FeedNews *fn = find_news(url);
    if (!fn) { fn = news_slot(); memset(fn, 0, sizeof(*fn)); strcpy(fn->url, url); }
    if (fn->n >= PER_FEED) continue;
    clean_text(titles[i], fn->h[fn->n].title, sizeof(fn->h[fn->n].title));
    fn->h[fn->n].when = time(nullptr) - mins[i] * 60;
    fn->n++;
    fn->fetched = time(nullptr) - 120;
  }
  version++;
}
void news_preview_open_manager(bool with_results, bool keyboard) {
  if (with_results) {
    snprintf(search_query, sizeof(search_query), "space");
    const char *names[] = {"NASA", "Space.com", "SpaceNews", "NASASpaceFlight.com"};
    const char *urls[] = {"http://www.nasa.gov/rss/breaking_news.rss", "https://www.space.com/feeds/all",
                          "https://spacenews.com/feed/", "http://www.nasaspaceflight.com/feed/"};
    for (int i = 0; i < 4; i++) { strcpy(results[i].name, names[i]); strcpy(results[i].url, urls[i]); }
    n_results = 4;
    search_state = 2;
    results_version++;
    feeds[n_feeds] = Feed();
    strcpy(feeds[n_feeds].name, "SpaceNews");
    strcpy(feeds[n_feeds].url, urls[2]);
    feeds[n_feeds].on = true;
    feeds[n_feeds].added = true;
    n_feeds++;
    feeds[1].on = false;
  }
  open_mgr(nullptr);
  if (keyboard) show_keyboard(true);
}
void news_preview_open_article(int row) {
  if (row_item[row]) {
    Headline *h = (Headline *)row_item[row];
    snprintf(h->link, sizeof(h->link), "https://www.npr.org/2026/10/22/nx-s1-5541234/senate-stopgap-funding-shutdown");
    snprintf(h->summary, sizeof(h->summary), "The bill keeps the government open through mid-December and gives negotiators more time to work out full-year spending, after a late deal between leaders of both parties.");
  }
  lv_event_t e;
  memset(&e, 0, sizeof(e));
  e.user_data = (void *)(intptr_t)row;
  open_article(&e);
}
void news_preview_close() { mgr = mgr_list = mgr_ta = mgr_kb = mgr_done = nullptr; search_state = 0; }
// Runs the feed parser over any Client (the preview feeds it a file)
int news_preview_parse(Client *c, char titles[][160], long *whens) {
  static Reader r;
  r = Reader();
  r.c = c;
  r.deadline = millis() + 15000;
  static FeedNews out;
  memset(&out, 0, sizeof(out));
  parse_feed(r, out);
  for (int i = 0; i < out.n; i++) {
    strcpy(titles[i], out.h[i].title);
    whens[i] = (long)out.h[i].when;
    printf("    link=%s | summary=%.60s\n", out.h[i].link, out.h[i].summary);
  }
  return out.n;
}
#endif
