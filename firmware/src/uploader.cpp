#include "uploader.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <SD.h>

int uploadFile(const Config &cfg, const String &path) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  File f = SD.open(path, FILE_READ);
  if (!f) return -2;
  size_t size = f.size();

  HTTPClient http;
  WiFiClient client;
  // For HTTPS, swap in WiFiClientSecure + setInsecure() or a pinned CA bundle.
  if (!http.begin(client, cfg.upload_url)) {
    f.close();
    return -3;
  }
  http.addHeader("Content-Type", "audio/wav");
  http.addHeader("Authorization", String("Bearer ") + cfg.api_key);
  http.addHeader("X-Device-Id", cfg.device_id);
  http.addHeader("X-Client-Timestamp", String((uint32_t)millis()));

  int code = http.sendRequest("POST", &f, size);
  http.end();
  f.close();
  return code;
}
