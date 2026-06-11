#include "config.h"
#include <Preferences.h>
#include <WiFi.h>

static const char *NS = "prawnd";

bool loadConfig(Config &cfg) {
  Preferences p;
  if (!p.begin(NS, true)) return false;
  cfg.ssid       = p.getString("ssid", "");
  cfg.psk        = p.getString("psk", "");
  cfg.upload_url = p.getString("url", "");
  cfg.api_key    = p.getString("key", "");
  cfg.device_id  = p.getString("dev", "");
  p.end();
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
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  char buf[16];
  snprintf(buf, sizeof(buf), "prawnd-%02X%02X", mac[4], mac[5]);
  return String(buf);
}
