#include "uploader.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <SD.h>

int uploadFile(const Config &cfg, const String &path) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  File f = SD.open(path, FILE_READ);
  if (!f) return -2;
  size_t size = f.size();

  const bool isHttps =
      cfg.upload_url.startsWith("https://") || cfg.upload_url.startsWith("HTTPS://");

  // Pick the transport by URL scheme. A plain WiFiClient pointed at an https://
  // URL opens a raw socket to :443 and writes cleartext, so a TLS endpoint (e.g.
  // the hosted prawnd.dev.cobl.io behind Cloudflare) replies "400 Bad Request -
  // The plain HTTP request was sent to HTTPS port". WiFiClientSecure does the
  // TLS handshake so uploads actually go through.
  //
  // Both clients live on the stack until http.end(); a pointer (not a reference
  // ternary) avoids object slicing of the WiFiClientSecure subobject.
  WiFiClient clientPlain;
  WiFiClientSecure clientSecure;
  if (isHttps) {
    // WARNING: disables TLS certificate validation (no MITM protection). This is
    // a beta convenience — see the security warning in README.md.
    clientSecure.setInsecure();
  }
  WiFiClient *client = isHttps ? static_cast<WiFiClient *>(&clientSecure) : &clientPlain;

  HTTPClient http;
  if (!http.begin(*client, cfg.upload_url)) {
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
