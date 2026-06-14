#include "battery.h"

#ifdef ENABLE_BATTERY
#include <Wire.h>
#include "pins.h"

// MAX17048 — ModelGauge, voltage-based fuel gauge (no current-sense resistor).
// We read two registers directly over I2C; no external library needed.
static constexpr uint8_t MAX17048_ADDR = 0x36;
static constexpr uint8_t REG_VCELL     = 0x02;  // 78.125 µV / LSB
static constexpr uint8_t REG_SOC       = 0x04;  // 1/256 % / LSB, high byte = integer %

static bool gaugePresent = false;

// Read a big-endian 16-bit register. Returns false on any bus error.
static bool readReg16(uint8_t reg, uint16_t &val) {
  Wire.beginTransmission(MAX17048_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;  // repeated-start
  if (Wire.requestFrom((int)MAX17048_ADDR, 2) != 2) return false;
  val  = (uint16_t)Wire.read() << 8;
  val |= (uint16_t)Wire.read();
  return true;
}

bool batteryBegin() {
  Wire.begin(PIN_BATT_SDA, PIN_BATT_SCL);
  Wire.beginTransmission(MAX17048_ADDR);
  gaugePresent = (Wire.endTransmission() == 0);
  return gaugePresent;
}

bool batteryRead(BatteryReading &out) {
  if (!gaugePresent) return false;
  uint16_t vcell, soc;
  if (!readReg16(REG_VCELL, vcell)) return false;
  if (!readReg16(REG_SOC,   soc))   return false;
  // 78.125 µV/LSB → mV. Widen to 64-bit: vcell*78125 overflows uint32 near full.
  out.millivolts = (int)((uint64_t)vcell * 78125 / 1000000);
  int pct = soc >> 8;  // high byte is the integer percent
  out.percent = pct > 100 ? 100 : pct;
  return true;
}

#else  // addon not compiled in — graceful no-ops

bool batteryBegin() { return false; }
bool batteryRead(BatteryReading &) { return false; }

#endif
