#pragma once
#include <Arduino.h>

void buttonBegin();
bool buttonShortPressed();   // single (or accidental double) click
bool buttonLongPressed();    // held >= 5 s (factory reset)
bool buttonTriplePressed();  // 3 quick clicks (enter BLE pairing mode)
