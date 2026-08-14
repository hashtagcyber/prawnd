#include "sd_mount.h"
#include <SD.h>
#include <SPI.h>
#include "pins.h"

static const uint32_t SD_IDLE_MS = 20000;

static bool     mounted = false;
static uint32_t lastUse = 0;

bool sdEnsureMounted() {
  lastUse = millis();
  if (mounted) return true;
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  // 1 MHz: see the note in main.cpp — recording headroom over the shared bus.
  if (!SD.begin(PIN_SD_CS, SPI, 1000000)) {
    SPI.end();
    return false;
  }
  mounted = true;
  Serial.println("[sd] mounted");
  return true;
}

bool sdIsMounted() { return mounted; }

void sdIdleMaintain(bool inhibit) {
  if (!mounted) return;
  if (inhibit) { lastUse = millis(); return; }
  if (millis() - lastUse < SD_IDLE_MS) return;
  SD.end();
  SPI.end();
  // Park the bus at defined levels so no pin back-feeds the card: CS high
  // (deselected), clock/data low, MISO pulled to a rail.
  pinMode(PIN_SD_CS, OUTPUT);   digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_SD_SCK, OUTPUT);  digitalWrite(PIN_SD_SCK, LOW);
  pinMode(PIN_SD_MOSI, OUTPUT); digitalWrite(PIN_SD_MOSI, LOW);
  pinMode(PIN_SD_MISO, INPUT_PULLUP);
  mounted = false;
  Serial.println("[sd] unmounted (idle)");
}
