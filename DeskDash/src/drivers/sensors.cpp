#include "sensors.h"
#include <Wire.h>

// ---------------------------------------------------------------------------
//  SHTC3 (address 0x70) — logic from Waveshare's 05_shtc3 example
// ---------------------------------------------------------------------------
static const uint8_t SHTC3_ADDR = 0x70;

static uint8_t shtc3_crc(const uint8_t *data, size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
  }
  return crc;
}

static bool shtc3_cmd(uint16_t cmd) {
  Wire.beginTransmission(SHTC3_ADDR);
  Wire.write(cmd >> 8);
  Wire.write(cmd & 0xFF);
  return Wire.endTransmission() == 0;
}

bool shtc3_begin() {
  if (!shtc3_cmd(0x3517)) return false;  // wake
  delayMicroseconds(240);
  shtc3_cmd(0x805D);                      // soft reset
  delay(2);
  return true;
}

bool shtc3_read(float *temp_c, float *humidity) {
  if (!shtc3_cmd(0x3517)) return false;  // wake
  delayMicroseconds(240);
  if (!shtc3_cmd(0x7866)) {               // measure, temperature first
    shtc3_cmd(0xB098);
    return false;
  }
  delay(20);
  uint8_t b[6];
  if (Wire.requestFrom(SHTC3_ADDR, (uint8_t)6) != 6) {
    shtc3_cmd(0xB098);
    return false;
  }
  for (int i = 0; i < 6; i++) b[i] = Wire.read();
  shtc3_cmd(0xB098);                      // sleep
  if (shtc3_crc(b, 2) != b[2] || shtc3_crc(b + 3, 2) != b[5]) return false;
  *temp_c   = -45.0f + 175.0f * (((uint16_t)b[0] << 8) | b[1]) / 65535.0f;
  *humidity = 100.0f * (((uint16_t)b[3] << 8) | b[4]) / 65535.0f;
  return true;
}

// ---------------------------------------------------------------------------
//  AXP2101 (address 0x34) — register map as used by XPowersLib
// ---------------------------------------------------------------------------
static const uint8_t AXP_ADDR = 0x34;

static bool axp_read(uint8_t reg, uint8_t *val) {
  Wire.beginTransmission(AXP_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(AXP_ADDR, (uint8_t)1) != 1) return false;
  *val = Wire.read();
  return true;
}

static bool axp_set_bit(uint8_t reg, uint8_t bit) {
  uint8_t v;
  if (!axp_read(reg, &v)) return false;
  Wire.beginTransmission(AXP_ADDR);
  Wire.write(reg);
  Wire.write(v | (1 << bit));
  return Wire.endTransmission() == 0;
}

bool pmu_begin() {
  // Only turn on measurements; never touch the power rails.
  bool ok = axp_set_bit(0x68, 0);   // battery detection
  ok &= axp_set_bit(0x30, 0);       // battery voltage ADC
  ok &= axp_set_bit(0x18, 3);       // fuel gauge
  Serial.printf("[pmu] AXP2101 %s\n", ok ? "ready" : "not found");
  return ok;
}

bool pmu_read(PowerInfo *p) {
  uint8_t s1, s2, pct, vh, vl;
  p->ok = axp_read(0x00, &s1) && axp_read(0x01, &s2);
  if (!p->ok) return false;
  p->battery  = s1 & (1 << 3);
  p->usb      = (s1 & (1 << 5)) && !(s2 & (1 << 3));
  p->charging = (s2 >> 5) == 0x01;
  p->percent  = -1;
  p->mv       = 0;
  if (p->battery) {
    if (axp_read(0xA4, &pct)) p->percent = pct > 100 ? 100 : pct;
    if (axp_read(0x34, &vh) && axp_read(0x35, &vl)) p->mv = ((vh & 0x1F) << 8) | vl;
  }
  return true;
}
