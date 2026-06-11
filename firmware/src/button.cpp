#include "button.h"
#include "pins.h"

static const uint32_t DEBOUNCE_MS = 30;
static const uint32_t LONG_MS = 5000;

static int lastReading  = HIGH;
static int stableState  = HIGH;
static uint32_t lastChangeAt = 0;
static uint32_t pressedAt    = 0;
static bool shortFlag = false;
static bool longFlag  = false;

void buttonBegin() {
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  lastReading = digitalRead(PIN_BUTTON);
  stableState = lastReading;
}

static void poll() {
  int reading = digitalRead(PIN_BUTTON);
  uint32_t t = millis();
  if (reading != lastReading) {
    lastChangeAt = t;
    lastReading = reading;
  }
  if ((t - lastChangeAt) > DEBOUNCE_MS && reading != stableState) {
    stableState = reading;
    if (stableState == LOW) {
      pressedAt = t;
    } else {
      uint32_t dur = t - pressedAt;
      if (dur >= LONG_MS)       longFlag  = true;
      else                       shortFlag = true;
    }
  }
}

bool buttonShortPressed() {
  poll();
  if (shortFlag) { shortFlag = false; return true; }
  return false;
}

bool buttonLongPressed() {
  poll();
  if (longFlag) { longFlag = false; return true; }
  return false;
}
