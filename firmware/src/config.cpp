#include "config.h"
#include <Preferences.h>
#include <WiFi.h>

static const char *NS = "prawnd";

bool loadConfig(Config &cfg) {
  Preferences p;
  // On a fresh/erased device the namespace doesn't exist yet and begin() fails;
  // that's fine — we still fall through and derive a device id below. (Don't
  // early-return here, or device_id stays empty and the BLE name / AP SSID come
  // up nameless.)
  if (p.begin(NS, true)) {
    cfg.ssid       = p.getString("ssid", "");
    cfg.psk        = p.getString("psk", "");
    cfg.upload_url = p.getString("url", "");
    cfg.api_key    = p.getString("key", "");
    cfg.device_id  = p.getString("dev", "");
    p.end();
  }
  if (!cfg.device_id.length()) cfg.device_id = defaultDeviceId();
  return cfg.valid();
}

bool saveConfig(const Config &cfg) {
  Preferences p;
  if (!p.begin(NS, false)) return false;
  p.putString("ssid", cfg.ssid);
  p.putString("psk", cfg.psk);
  p.putString("url", cfg.upload_url);
  p.putString("key", cfg.api_key);
  p.putString("dev", cfg.device_id.length() ? cfg.device_id : defaultDeviceId());
  p.end();
  return true;
}

void clearConfig() {
  Preferences p;
  if (p.begin(NS, false)) {
    p.clear();
    p.end();
  }
}

String defaultDeviceId() {
  // Use the factory eFuse MAC, not WiFi.macAddress(): this runs at boot before
  // WiFi is initialized, where WiFi.macAddress() reads back all-zeros.
  uint64_t mac = ESP.getEfuseMac();
  char buf[16];
  snprintf(buf, sizeof(buf), "prawnd-%04X", (uint16_t)(mac >> 32));
  return String(buf);
}
