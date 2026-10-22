// ============================================================================
//  DeskDash — a swipeable desk dashboard for the
//  Waveshare ESP32-C5-Touch-LCD-3.5 (320 x 480, ST7796 + FT6336)
//
//  Every page is a plugin in src/plugins/<name>/. To add, remove or reorder pages,
//  edit the PAGES list below. See README.md ("Writing a plugin") to make your own.
//
//  Settings (Wi-Fi, location, units, ...) are in config.h.
// ============================================================================
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include "src/core/deskdash.h"        // plugin API (pulls in the bundled LVGL 8.4 and config.h)
#include "src/core/core_internal.h"
#include "src/drivers/Board_IO.h"
#include "src/drivers/Touch_FT6336.h"
#include "src/drivers/lcd_st7796.h"
#include "src/drivers/sensors.h"

// ---- Pages, in swipe order. Comment a line out to hide that page. -------------------
extern const DeskDashPlugin plugin_clock, plugin_weather, plugin_arrivals, plugin_departures,
                            plugin_news, plugin_system;

static const DeskDashPlugin *PAGES[] = {
  &plugin_clock,        // time, date, indoor reading, current weather
  &plugin_weather,      // forecast (also feeds the clock page's weather row)
  &plugin_arrivals,     // flights arriving at your airport (tap title to change airport)
  &plugin_departures,   // flights leaving your airport
  &plugin_news,         // headlines from RSS feeds (tap title to choose feeds)
  &plugin_system,       // Wi-Fi, battery, uptime, brightness
};
static const int PAGE_COUNT = sizeof(PAGES) / sizeof(PAGES[0]);

// ---- Core state ------------------------------------------------------------------------
static lv_obj_t *tileview;
static lv_obj_t *dots[PAGE_COUNT];
static uint8_t current_brightness = 255;
static volatile uint32_t last_touch_ms = 0;
static bool touch_ok = false;

// ============================================================================
//  Page indicator and tileview
// ============================================================================
static void update_dots(int active) {
  for (int i = 0; i < PAGE_COUNT; i++) {
    lv_obj_set_style_bg_color(dots[i], i == active ? COL_TEXT : COL_FAINT, 0);
    lv_obj_set_width(dots[i], i == active ? 20 : 8);
  }
}

static uint32_t page_shown_ms = 0;     // when the current page appeared (for auto-cycling)

static volatile int current_page = 0;

static void tileview_changed_cb(lv_event_t *e) {
  page_shown_ms = millis();
  current_page = lv_obj_get_x(lv_tileview_get_tile_act(tileview)) / SCREEN_W;
  update_dots(current_page);
}

static void build_ui() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, COL_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  tileview = lv_tileview_create(scr);
  lv_obj_set_size(tileview, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_opa(tileview, LV_OPA_TRANSP, 0);
  lv_obj_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(tileview, tileview_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

  for (int i = 0; i < PAGE_COUNT; i++) {
    lv_obj_t *tile = lv_tileview_add_tile(tileview, i, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    if (PAGES[i]->build) PAGES[i]->build(tile);
  }

  lv_obj_t *dot_row = lv_obj_create(scr);
  lv_obj_remove_style_all(dot_row);
  lv_obj_set_size(dot_row, LV_SIZE_CONTENT, 8);
  lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(dot_row, 8, 0);
  lv_obj_align(dot_row, LV_ALIGN_BOTTOM_MID, 0, -22);
  lv_obj_clear_flag(dot_row, LV_OBJ_FLAG_CLICKABLE);
  for (int i = 0; i < PAGE_COUNT; i++) {
    dots[i] = lv_obj_create(dot_row);
    lv_obj_remove_style_all(dots[i]);
    lv_obj_set_size(dots[i], 8, 8);
    lv_obj_set_style_radius(dots[i], 4, 0);
    lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
  }
  update_dots(0);
}

// ============================================================================
//  Timers on the LVGL thread
// ============================================================================
// Backlight is driven by the CH32V006 expander over I2C; only call on the LVGL thread.
static void set_brightness(uint8_t pct) {
  if (pct != current_brightness) {
    Board_SetBacklight(pct);
    current_brightness = pct;
  }
}

// Every second: plugin ticks, then brightness (dim at night; a tap wakes it for a while)
static void tick_timer_cb(lv_timer_t *t) {
  for (int i = 0; i < PAGE_COUNT; i++)
    if (PAGES[i]->tick) PAGES[i]->tick();
  bool recently_touched = last_touch_ms && (millis() - last_touch_ms < WAKE_SECONDS * 1000UL);
  set_brightness(is_night() && !recently_touched ? NIGHT_BRIGHTNESS : dd_day_brightness());

  // Move to the next page every AUTO_CYCLE_SECONDS so no image sits on the panel for long.
  // Waits while someone is using the screen (touched within the last cycle) or a pop-up
  // such as the airport picker is open.
  if (AUTO_CYCLE_SECONDS > 0 && PAGE_COUNT > 1) {
    uint32_t cycle_ms = AUTO_CYCLE_SECONDS * 1000UL;
    bool in_use = last_touch_ms && (millis() - last_touch_ms < cycle_ms);
    bool popup = lv_obj_get_child_cnt(lv_layer_top()) > 0;
    if (in_use || popup) {
      page_shown_ms = millis();
    } else if (millis() - page_shown_ms >= cycle_ms) {
      int cur = lv_obj_get_x(lv_tileview_get_tile_act(tileview)) / SCREEN_W;
      int next = (cur + 1) % PAGE_COUNT;
      lv_obj_set_tile_id(tileview, next, 0, next == 0 ? LV_ANIM_OFF : LV_ANIM_ON);
      update_dots(next);
      page_shown_ms = millis();
    }
  }
}

// Every 15 s: battery and indoor sensor (all I2C stays on this thread)
static void sensor_timer_cb(lv_timer_t *t) {
  PowerInfo p;
  if (pmu_read(&p)) core_set_power({p.ok, p.battery, p.charging, p.usb, p.percent, p.mv});
  float c, rh;
  if (shtc3_read(&c, &rh)) core_set_indoor(c, rh);
}

// ============================================================================
//  LVGL glue: display flush, touch input, and the LVGL task
// ============================================================================
static void display_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  size_t bytes = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1) * sizeof(lv_color_t);
  if (!lcd_send_area(area->x1, area->y1, area->x2, area->y2, color_p, bytes)) Serial.println("[lcd] transfer failed");
  lv_disp_flush_ready(drv);
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  static uint16_t last_x = SCREEN_W / 2, last_y = SCREEN_H / 2;
  uint16_t x[2] = {}, y[2] = {};
  uint8_t count = 0;
  if (touch_ok && FT6336_Read(x, y, &count, 1) && count) {
    last_x = x[0];
    last_y = y[0];
    data->state = LV_INDEV_STATE_PR;
    last_touch_ms = millis();
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
  data->point.x = last_x;
  data->point.y = last_y;
}

static void lvgl_task(void *arg) {
  uint32_t last = millis();
  while (true) {
    uint32_t now = millis();
    uint32_t wait = 5;
    if (ui_lock()) {
      lv_tick_inc(now - last);
      wait = lv_timer_handler();
      ui_unlock();
    }
    last = now;
    vTaskDelay(pdMS_TO_TICKS(wait < 2 ? 2 : (wait > 20 ? 20 : wait)));
  }
}

// ============================================================================
//  Wi-Fi
// ============================================================================
// Lists every network the radio can see, so a missing or misnamed network is obvious in the log.
static void wifi_scan_report() {
  int n = WiFi.scanNetworks();
  Serial.printf("[wifi] scan found %d networks:\n", n);
  for (int i = 0; i < n && i < 20; i++)
    Serial.printf("[wifi]   \"%s\"  ch %d  %d dBm%s\n", WiFi.SSID(i).c_str(), (int)WiFi.channel(i),
                  (int)WiFi.RSSI(i), WiFi.SSID(i) == WIFI_SSID ? "   <-- yours" : "");
  WiFi.scanDelete();
}

// ============================================================================
//  Arduino entry points
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\nDeskDash starting, %d pages (last reset: %d)\n", PAGE_COUNT, (int)esp_reset_reason());

  // Board power, resets and backlight live on the CH32V006 I/O expander
  if (!Board_IO_Init(Wire)) Serial.println("[board] CH32V006 I/O expander not found");
  Board_SetBacklight(0);
  touch_ok = FT6336_Init();   // resets touch; must happen before the LCD init
  Board_LCD_Reset();
  if (!lcd_begin()) Serial.println("[lcd] init failed");

  pmu_begin();
  float c, rh;
  if (shtc3_begin() && shtc3_read(&c, &rh)) core_set_indoor(c, rh);
  core_load_brightness();

  // LVGL draw buffers in internal DMA-capable RAM, allocated before Wi-Fi starts
  const size_t buf_px = SCREEN_W * 40;
  lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!buf1 || !buf2) Serial.println("[lvgl] draw buffer allocation failed");

  lv_init();
  static lv_disp_draw_buf_t draw_buf;
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_px);
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_W;
  disp_drv.ver_res = SCREEN_H;
  disp_drv.flush_cb = display_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read_cb;
  lv_indev_drv_register(&indev_drv);

  core_init_lock();
  ui_lock();
  build_ui();
  sensor_timer_cb(nullptr);
  tick_timer_cb(nullptr);
  lv_timer_create(tick_timer_cb, 1000, NULL);
  lv_timer_create(sensor_timer_cb, 15000, NULL);
  lv_refr_now(NULL);
  ui_unlock();
  xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 2, NULL);

  // Wi-Fi and time
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
      Serial.printf("[wifi] disconnected, reason %d (%s), rssi %d, page %s, up %lus, free internal %u\n",
                    info.wifi_sta_disconnected.reason,
                    WiFi.disconnectReasonName((wifi_err_reason_t)info.wifi_sta_disconnected.reason),
                    info.wifi_sta_disconnected.rssi, PAGES[current_page]->name, (unsigned long)(millis() / 1000),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
      Serial.printf("[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());
  });
  WiFi.mode(WIFI_STA);
  WiFi.setBandMode(WIFI_BAND_MODE_AUTO);   // 2.4 GHz and 5 GHz
  wifi_scan_report();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTzTime(TIMEZONE, "pool.ntp.org", "time.nist.gov");
}

// Health line every 30 s, to diagnose drops: memory, Wi-Fi state and which page is showing
static void diag_report() {
  static uint32_t last = 0;
  if (millis() - last < 30000) return;
  last = millis();
  Serial.printf("[diag] up %lus, page %s, wifi %s %d dBm ch %d, internal free %u min %u largest %u, psram free %u\n",
                (unsigned long)(millis() / 1000), PAGES[current_page]->name,
                WiFi.status() == WL_CONNECTED ? "up" : "DOWN", (int)WiFi.RSSI(), (int)WiFi.channel(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void loop() {
  diag_report();
  static uint32_t wifi_lost_since = 0;
  if (WiFi.status() == WL_CONNECTED) {
    wifi_lost_since = 0;
    for (int i = 0; i < PAGE_COUNT; i++)
      if (PAGES[i]->poll) PAGES[i]->poll();
  } else {
    if (wifi_lost_since == 0) wifi_lost_since = millis();
    if (millis() - wifi_lost_since > 30000) {   // retry every 30 s
      Serial.printf("[wifi] not connected (status %d), retrying\n", (int)WiFi.status());
      WiFi.disconnect();
      delay(200);
      wifi_scan_report();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      wifi_lost_since = millis();
    }
  }
  delay(200);
}
