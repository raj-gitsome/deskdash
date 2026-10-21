// Live arrivals into one airport, from the free adsb.lol API (community ADS-B network).
// 1. /v2/point/<lat>/<lon>/<nm> lists every aircraft near the airport.
// 2. /api/0/routeset looks up each new callsign's route; flights whose route ends
//    at the airport are arrivals. Routes are cached so each callsign is looked up once.
// 3. Many flights have no published route (private jets, some airline legs). Those count as
//    arriving when they are pointed at the field, descending, and low enough for their distance.
#pragma once
#include <Arduino.h>

#define MAX_ARRIVALS 6

struct Arrival {
  char callsign[9];     // e.g. "JBU1234"
  char display[24];     // e.g. "JetBlue 1234"
  char type[5];         // aircraft type, e.g. "A320"
  char origin[4];       // IATA code, "" if unknown
  char origin_city[20];
  float dist_nm;
  int alt_ft;
  int eta_min;          // straight-line estimate at fetch time
  time_t eta;           // estimated arrival clock time (0 if unknown)
};

struct Arrivals {
  int n = 0;
  int nearby = 0;       // all aircraft seen in the radius
  Arrival items[MAX_ARRIVALS];
  time_t updated = 0;
};

// Fills `out` with up to MAX_ARRIVALS inbound flights, nearest ETA first. Returns false on a network error.
bool flights_fetch(const char *airport_icao, double lat, double lon, int radius_nm, Arrivals *out);
