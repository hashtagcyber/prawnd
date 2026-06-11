#pragma once
#include <Arduino.h>

struct Config {
  String ssid;
  String psk;
  String upload_url;
  String api_key;
  String device_id;
  bool valid() const { return ssid.length() > 0 && upload_url.length() > 0; }
};

bool loadConfig(Config &cfg);
bool saveConfig(const Config &cfg);
void clearConfig();
String defaultDeviceId();
