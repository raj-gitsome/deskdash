// Network helpers that keep large buffers out of internal RAM.
//
// The ESP32-C5 has about 80 KB of internal RAM free once Wi-Fi and the display are running,
// and Wi-Fi itself must allocate from it. A 60 KB JSON response read into a String, or a
// JSON document built with the default allocator, can use nearly all of it; when that
// happens Wi-Fi can't allocate its buffers and the board drops off the network or crashes.
// These helpers put response bodies and JSON documents in PSRAM instead.
#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Allocator for JsonDocument that uses PSRAM:  JsonDocument doc(dd_psram_json());
ArduinoJson::Allocator *dd_psram_json();

// A response body held in PSRAM. Empty (data == nullptr) if the read failed.
struct PsramBody {
  char *data = nullptr;
  size_t len = 0;
  PsramBody() = default;
  PsramBody(const PsramBody &) = delete;
  PsramBody &operator=(const PsramBody &) = delete;
  ~PsramBody();
};

// Reads the body of a request that returned a status (handles chunked encoding).
// Stops at max_bytes. Returns false if nothing was read.
bool dd_read_body(HTTPClient &http, PsramBody &out, size_t max_bytes = 256 * 1024);

// Logs where large allocations land and any failed allocation.
void dd_mem_watch_begin();

// Moves TLS (HTTPS) buffers to PSRAM. Each HTTPS connection otherwise takes ~65 KB of the
// ~80 KB internal RAM that Wi-Fi also needs. Call once in setup(), before any HTTPS.
void dd_tls_use_psram();
