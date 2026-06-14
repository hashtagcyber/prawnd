#pragma once
#include <Arduino.h>

// Optional battery fuel-gauge addon (MAX17048 over I2C).
//
// When the firmware is built WITHOUT -DENABLE_BATTERY these are compiled as
// no-ops that report "no gauge present", so callers (main, portal) can invoke
// them unconditionally without #ifdefs of their own.
struct BatteryReading {
  int millivolts;  // cell voltage, mV
  int percent;     // state-of-charge, 0..100
};

// Init I2C and probe for the gauge. Returns true if a gauge ACKs on the bus.
// Always false in builds without the addon compiled in.
bool batteryBegin();

// Populate `out` with the latest reading. Returns false if no gauge is present
// or the I2C read failed (callers should then omit battery fields entirely).
bool batteryRead(BatteryReading &out);
