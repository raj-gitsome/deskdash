#include "net.h"
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <mbedtls/platform.h>

namespace {
struct SpiRamAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
  }
  void deallocate(void *p) override { free(p); }
  void *reallocate(void *p, size_t n) override {
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, n);
  }
};
SpiRamAllocator spiram_allocator;

// A Stream that appends everything written to it into a growing PSRAM buffer
class PsramSink : public Stream {
 public:
  PsramSink(PsramBody &b, size_t max) : body(b), limit(max) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *buf, size_t n) override {
    if (body.len + n > limit) n = limit > body.len ? limit - body.len : 0;
    if (n == 0) return 0;
    if (body.len + n + 1 > cap) {
      size_t want = cap ? cap * 2 : 16384;
      while (want < body.len + n + 1) want *= 2;
      if (want > limit + 1) want = limit + 1;
      char *p = (char *)heap_caps_realloc(body.data, want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!p) return 0;
      body.data = p;
      cap = want;
    }
    memcpy(body.data + body.len, buf, n);
    body.len += n;
    body.data[body.len] = 0;
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

 private:
  PsramBody &body;
  size_t limit;
  size_t cap = 0;
};

void failed_alloc(size_t size, uint32_t caps, const char *function) {
  Serial.printf("[mem] allocation of %u bytes failed (caps 0x%x) in %s; internal free %u\n", (unsigned)size,
                (unsigned)caps, function ? function : "?", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
}  // namespace

static void *tls_calloc(size_t n, size_t size) {
  size_t total = n * size;
  if (total >= 512) {
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
  }
  return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void tls_free(void *p) { heap_caps_free(p); }

void dd_tls_use_psram() { mbedtls_platform_set_calloc_free(tls_calloc, tls_free); }

ArduinoJson::Allocator *dd_psram_json() { return &spiram_allocator; }

PsramBody::~PsramBody() { free(data); }

bool dd_read_body(HTTPClient &http, PsramBody &out, size_t max_bytes) {
  PsramSink sink(out, max_bytes);
  int n = http.writeToStream(&sink);
  if (out.len == 0) {
    if (n < 0) Serial.printf("[net] read failed: %s\n", http.errorToString(n).c_str());
    return false;
  }
  return true;
}

void dd_mem_watch_begin() {
  heap_caps_register_failed_alloc_callback(failed_alloc);
  void *probe = malloc(32768);
  Serial.printf("[mem] internal free %u, psram free %u; a plain 32 KB malloc lands in %s\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                probe ? (esp_ptr_external_ram(probe) ? "PSRAM" : "internal RAM") : "nowhere (failed)");
  free(probe);
}
