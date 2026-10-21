// Departures: flights taxiing out of or climbing away from the selected airport
// (tap the title to change airport). Shares data with the Arrivals plugin, so it costs
// no extra requests.
#include "../flights/flights.h"

static FlightPage page;

static void build(lv_obj_t *p) { flights_build_page(page, p, false); }
static void tick() { flights_tick_page(page); }
static void poll() { flights_poll(); }   // safe to call from both plugins; fetches once per interval

extern const DeskDashPlugin plugin_departures = {"Departures", build, tick, poll};
