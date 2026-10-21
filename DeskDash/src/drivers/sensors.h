// Small drivers for the on-board SHTC3 (indoor temperature/humidity) and
// AXP2101 (battery and USB power status). Both live on the I2C bus at SDA 27 / SCL 26.
#pragma once
#include <Arduino.h>

struct PowerInfo {
  bool ok;            // PMU answered
  bool battery;       // a battery is attached
  bool charging;
  bool usb;           // USB power present
  int percent;        // 0-100, -1 if unknown
  uint16_t mv;        // battery voltage
};

bool shtc3_begin();
bool shtc3_read(float *temp_c, float *humidity);

bool pmu_begin();
bool pmu_read(PowerInfo *info);
