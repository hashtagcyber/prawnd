#pragma once
#include <Arduino.h>

// BLE sync transport. Advertises a GATT service that lets a companion phone app
// (the sole uplink, post-WiFi-removal) pull queued recordings off the SD card
// and relay them to the prawnd server over the phone's internet. The device
// drains its on-SD /pending -> /uploaded queue purely over BLE.
//
// Wire contract: see docs/ble-autosync-spec.md Section B (the binding boundary).
// STAT carries tab-delimited text status lines; DATA carries binary framed file
// chunks {seq:u32, len:u16, payload}; CTRL carries text commands plus a binary
// windowed-ACK (first byte 0x01).
//
// NimBLE callbacks run in the BLE host task, so they only enqueue a command /
// ACK; the actual SD reads + notifications happen in bleSyncService(), called
// from the main loop, which keeps all SD access single-threaded.

// Initialise NimBLE, the GATT service (CTRL/STAT/DATA/CFG), security (Just-Works
// bonding + Secure Connections), and start advertising. `deviceId` becomes the
// advertised name.
void bleSyncBegin(const String &deviceId);

// Cooperative service step: advances the pairing-window timer, applies a pending
// CTRL command (LIST/GET/ACK/ABORT/WIN/CFG), and pumps the windowed GET sender.
// Call from the main loop while Idle. Never blocks the loop more than a few ms.
void bleSyncService();

// True while a phone is connected AND a GET/transfer is active.
bool bleSyncBusy();

// Open a ~60 s pairing window (triggered by a triple button press). New phones
// can bond only during this window (no PIN — "Just Works"); outside it the
// device still advertises and still accepts reconnections from already-bonded
// phones, but rejects brand-new bonds.
void bleSyncEnterPairing();

// True while the pairing window is open (for status / LED).
bool bleSyncPairing();

// Apply a new device id received over the CFG characteristic: updates the
// advertised name and re-advertises (no reboot). Persistence is the caller's job
// (the CFG write path calls saveDeviceId()).
void bleSyncSetDeviceId(const String &deviceId);

// Recompute advertising flags/interval for the given pending count and battery
// percent (0xFF = unknown). Sets the PENDING flag bit when count>0 and bumps to
// the fast advertising interval while pending or pairing. Call from the main
// loop only — never from a BLE callback.
void bleSyncUpdatePending(uint32_t pendingCount, uint8_t batteryPct);
