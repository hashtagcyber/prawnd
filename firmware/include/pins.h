#pragma once
#include <Arduino.h>

// I2S (shared bus for both SPH0645 mics)
constexpr int PIN_I2S_BCLK = 2;   // D2
constexpr int PIN_I2S_WS   = 1;   // D1
constexpr int PIN_I2S_DIN  = 0;   // D0

// SD card (SPI)
constexpr int PIN_SD_SCK   = 19;  // D8
constexpr int PIN_SD_MISO  = 20;  // D9
constexpr int PIN_SD_MOSI  = 18;  // D10
constexpr int PIN_SD_CS    = 21;  // D3

// Button (active-low with internal pull-up)
constexpr int PIN_BUTTON   = 17;  // D7
