#pragma once
#include <Arduino.h>

// Lazy SD mount with idle unmount (power: an idle mounted card draws
// 0.5-2 mA continuously). Callers ask for the card only when they need it;
// the main loop unmounts after SD_IDLE_MS without use, unless inhibited
// (recording in progress / phone connected).

// Mount if needed and stamp last-use. Returns false if no card responds.
bool sdEnsureMounted();

// True while the card is mounted.
bool sdIsMounted();

// Call once per main-loop pass. `inhibit` keeps the card mounted (and
// refreshes last-use) while recording or while a phone is connected.
void sdIdleMaintain(bool inhibit);
