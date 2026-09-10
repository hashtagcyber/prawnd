#pragma once
#include <Arduino.h>

// I2S (shared bus for both INMP441 mics)
constexpr int PIN_I2S_BCLK = 2;   // D2
constexpr int PIN_I2S_WS   = 1;   // D1
constexpr int PIN_I2S_DIN  = 0;   // D0

// SD card (SPI). SCK/MISO are swapped from the C6 defaults via the GPIO
// matrix: carrier rev 2 routes J7 crossing-free this way. (Rev 1 boards and
// the original point-to-point wiring used SCK=19/MISO=20.)
constexpr int PIN_SD_SCK   = 20;  // D9
constexpr int PIN_SD_MISO  = 19;  // D8
constexpr int PIN_SD_MOSI  = 18;  // D10
constexpr int PIN_SD_CS    = 21;  // D3

// Button (active-low with internal pull-up)
constexpr int PIN_BUTTON   = 17;  // D7

// Recording indicator LED (external, D6 -> series resistor -> LED -> GND).
// Driven HIGH while recording, LOW otherwise.
constexpr int PIN_REC_LED  = 16;  // D6

// Battery fuel gauge (optional addon — MAX17048 over I2C).
// Only used when the firmware is built with -DENABLE_BATTERY
// (the `xiao_esp32c6_batt` PlatformIO env). These are the XIAO's default
// I2C pins and are otherwise unused by the base firmware.
constexpr int PIN_BATT_SDA = 22;  // D4
constexpr int PIN_BATT_SCL = 23;  // D5
