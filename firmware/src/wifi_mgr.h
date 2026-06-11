#pragma once
#include <Arduino.h>

bool   wifiTryStation(const String &ssid, const String &psk, uint32_t timeoutMs = 15000);
void   wifiStartAp(const String &deviceId);
bool   wifiInStaMode();
String wifiCurrentIp();
