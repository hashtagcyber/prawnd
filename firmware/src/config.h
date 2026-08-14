#pragma once
#include <Arduino.h>

// Device configuration. WiFi/server fields were removed in the BLE auto-sync
// rearchitecture — the phone holds the server credentials now, and the only
// configurable surface is the device id (set over the CFG BLE characteristic).
struct Config {
  String device_id;  // NVS key "dev"
  bool valid() const { return device_id.length() > 0; }
};

bool loadConfig(Config &cfg);
bool saveConfig(const Config &cfg);
void clearConfig();
String defaultDeviceId();

// Persist just the device id (used by the CFG write path). Validation of the
// id format is done by the caller. Returns true on a successful NVS write.
bool saveDeviceId(const String &deviceId);
