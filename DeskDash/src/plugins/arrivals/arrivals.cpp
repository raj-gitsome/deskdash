// PBI Arrivals: flights inbound to the selected airport (tap the title to change airport).
// Data and the airport picker live in ../flights/, shared with the Departures plugin.
#include "../flights/flights.h"

static FlightPage page;

static void build(lv_obj_t *p) { flights_build_page(page, p, true); }
static void tick() { flights_tick_page(page); }
static void poll() { flights_poll(); }

extern const DeskDashPlugin plugin_arrivals = {"Arrivals", build, tick, poll};
