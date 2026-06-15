#include "button.h"
#include "pins.h"

static const uint32_t DEBOUNCE_MS   = 30;
static const uint32_t LONG_MS       = 5000;
static const uint32_t MULTICLICK_MS = 500;  // max gap between clicks in a burst

static int lastReading  = HIGH;
static int stableState  = HIGH;
static uint32_t lastChangeAt = 0;
static uint32_t pressedAt    = 0;

// A short release adds a click to the current burst; the burst is resolved once
// no further click arrives within MULTICLICK_MS. This lets a triple-click be a
// distinct gesture without the first click also firing as a single press — at
// the cost of ~MULTICLICK_MS latency on a single press, which is fine here.
static uint8_t  clickCount  = 0;
static uint32_t lastClickAt = 0;
static bool shortFlag  = false;
static bool longFlag   = false;
static bool tripleFlag = false;

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
      if (dur >= LONG_MS) {
        longFlag = true;
        clickCount = 0;   // a long hold isn't part of a click burst
      } else {
        clickCount++;
        lastClickAt = t;
      }
    }
  }
  // Resolve the burst once it's been quiet for MULTICLICK_MS.
  if (clickCount > 0 && (t - lastClickAt) > MULTICLICK_MS) {
    if (clickCount >= 3) tripleFlag = true;
    else                 shortFlag  = true;  // 1 or 2 clicks -> one short press
    clickCount = 0;
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

bool buttonTriplePressed() {
  poll();
  if (tripleFlag) { tripleFlag = false; return true; }
  return false;
}
