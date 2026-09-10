// Minimal blink for the carrier board's D1 recording LED ONLY (GPIO16/D6).
// The XIAO's built-in yellow user LED (GPIO15, active-LOW) is forced OFF the
// whole time, so any light you see blinking is the external 10mm LED.
//
//   pio run -e blinktest -t upload && pio device monitor
#include <Arduino.h>
#include "pins.h"

static const int PIN_BUILTIN_LED = 15;  // XIAO ESP32-C6 user LED, active-low

void setup() {
  pinMode(PIN_BUILTIN_LED, OUTPUT);
  digitalWrite(PIN_BUILTIN_LED, HIGH);  // active-low: HIGH = off

  pinMode(PIN_REC_LED, OUTPUT);
  digitalWrite(PIN_REC_LED, LOW);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  Serial.println("[blinktest] blinking ONLY GPIO16 (D6 -> R1 -> D1). Built-in LED held off.");
  Serial.println("[blinktest] if nothing blinks on the board, suspect D1 polarity/joints.");
}

void loop() {
  static uint32_t n = 0;
  digitalWrite(PIN_REC_LED, HIGH);
  Serial.printf("[blinktest] %lu ON  (GPIO16 high)\n", (unsigned long)++n);
  delay(500);
  digitalWrite(PIN_REC_LED, LOW);
  Serial.println("[blinktest] OFF");
  delay(500);
}
