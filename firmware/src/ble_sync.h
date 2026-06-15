#pragma once
#include <Arduino.h>

// BLE sync transport. Advertises a GATT service that lets a companion phone app
// pull queued recordings off the SD card and relay them to the prawnd server
// over the phone's internet — an alternate uplink to the WiFi uploader. Both
// drain the same on-SD /pending -> /uploaded queue.
//
// NimBLE callbacks run in the BLE host task, so they only enqueue a command;
// the actual SD reads + notifications happen in bleSyncService(), called from
// the main loop, which keeps all SD access single-threaded.
void bleSyncBegin(const String &deviceId);

// Process at most one pending phone command (LIST/GET/ACK). Call from the main
// loop while idle. A GET streams the whole file and blocks the loop for the
// transfer duration (a few seconds) — acceptable for a user-initiated sync.
void bleSyncService();

// True while a phone is connected. Lets the main loop hold off on churning the
// WiFi upload path while a BLE sync is in progress.
bool bleSyncBusy();

// Open a ~60 s pairing window (triggered by a triple button press). New phones
// can bond only during this window (no PIN — "Just Works"); outside it the
// device only accepts already-bonded phones. While the window is open the
// device advertises a "pairing" flag the app's pair screen looks for.
void bleSyncEnterPairing();

// True while the pairing window is open (for status / LED).
bool bleSyncPairing();
