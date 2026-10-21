// Shared flight data for the Arrivals and Departures plugins.
//
// Data (free community ADS-B networks, no keys):
//  1. adsb.fi /api/v3/lat/<lat>/lon/<lon>/dist/<nm> lists every aircraft near the airport
//     (1 request/second allowed), polled every ARRIVALS_REFRESH_SECONDS.
//  2. adsb.lol /api/0/routeset looks up each new callsign's route (cached per callsign,
//     batched at most every 10 minutes because adsb.lol allows ~500 requests/day).
//  3. A flight whose route ends at the airport is an arrival; one whose route starts there
//     is a departure. Flights with no published route (private jets, some airline legs) are
//     judged by their flight path: descending toward the field, or climbing away from it.
//
// Arrivals are tracked between fetches: when one touches down (seen on the ground at the
// airport, or lost from coverage while low on final) it stays listed as "Landed" for
// LANDED_KEEP_MINUTES.
//
// One fetch feeds both pages. The airport is chosen on screen (tap either page's title)
// and saved on the board; AIRPORT_CODE in config.h is only the first-boot default.
#pragma once
#include "../../core/deskdash.h"

#define MAX_FLIGHTS 6

struct Airport {
  const char *iata;
  const char *icao;
  const char *name;    // short, fits the page title
  float lat, lon;
};

struct Flight {
  char callsign[9];      // e.g. "JBU1234"
  char display[24];      // e.g. "JetBlue 1234"
  char type[5];          // aircraft type, e.g. "A320"
  char other[4];         // IATA of the origin (arrivals) or destination (departures), "" if unknown
  char other_city[20];
  float dist_nm;
  int alt_ft;
  bool on_ground;        // departures only: taxiing out
  bool landed;           // arrivals only: has touched down
  time_t when;           // arrivals: estimated arrival (or landing time); departures: estimated takeoff; 0 = unknown
};

struct FlightBoard {
  int n_arr = 0, n_dep = 0, nearby = 0;
  Flight arr[MAX_FLIGHTS];
  Flight dep[MAX_FLIGHTS];
  time_t updated = 0;
};

// Airports offered in the picker
int flights_airport_count();
const Airport &flights_airport_at(int i);
const Airport &flights_airport();          // the selected one

const FlightBoard &flights_board();        // read on the UI thread
uint32_t flights_version();                // changes on new data or a new airport

void flights_poll();                       // from a plugin's poll(); fetches when due

// Page UI shared by both plugins (UI thread)
struct FlightPage {
  bool arrivals;
  lv_obj_t *title, *status, *empty;
  lv_obj_t *name[MAX_FLIGHTS], *time[MAX_FLIGHTS], *info[MAX_FLIGHTS];
  uint32_t shown = 0xFFFFFFFF;
};
void flights_build_page(FlightPage &pg, lv_obj_t *parent, bool arrivals);
void flights_tick_page(FlightPage &pg);
