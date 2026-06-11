#include "wifi_mgr.h"
#include <WiFi.h>

static bool inSta = false;

bool wifiTryStation(const String &ssid, const String &psk, uint32_t timeoutMs) {
  if (!ssid.length()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), psk.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(200);
  }
  inSta = WiFi.status() == WL_CONNECTED;
  return inSta;
}

void wifiStartAp(const String &deviceId) {
  WiFi.mode(WIFI_AP);
  String tail = deviceId.length() >= 4
                  ? deviceId.substring(deviceId.length() - 4)
                  : String("0000");
  String ssid = String("Prawnd-") + tail;
  WiFi.softAP(ssid.c_str(), "prawnd1234");
  inSta = false;
}

bool   wifiInStaMode() { return inSta; }
String wifiCurrentIp() {
  return inSta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
}
