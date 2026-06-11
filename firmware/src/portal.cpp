#include "portal.h"
#include <ESPAsyncWebServer.h>
#include <SD.h>
#include <WiFi.h>

static AsyncWebServer server(80);
static Config *configPtr = nullptr;
static volatile bool saveRequested = false;

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Prawnd config</title>
<style>body{font-family:sans-serif;max-width:480px;margin:1rem auto;padding:1rem}
label{display:block;margin:.6rem 0 .2rem}
input{width:100%;padding:.5rem;box-sizing:border-box;font:inherit}
button{margin-top:1rem;padding:.6rem 1rem;font:inherit}
code{background:#eef;padding:.1rem .3rem;border-radius:3px}</style>
</head><body>
<h1>Prawnd</h1>
<p>Device <code id="d">?</code> &middot; mode <code id="s">?</code> &middot; ip <code id="i">?</code></p>
<form method="POST" action="/save">
<label>WiFi SSID</label><input name="ssid" value="__SSID__" required>
<label>WiFi password (leave blank to keep current)</label><input name="psk" type="password" placeholder="********">
<label>Upload URL</label><input name="upload_url" value="__URL__" required placeholder="http://host:8080/upload">
<label>API key (leave blank to keep current)</label><input name="api_key" type="password" placeholder="********">
<label>Device id</label><input name="device_id" value="__DEV__">
<button type="submit">Save and reboot</button>
</form>
<script>
fetch('/status').then(r=>r.json()).then(j=>{
  document.getElementById('d').textContent=j.device_id||'?';
  document.getElementById('s').textContent=j.mode||'?';
  document.getElementById('i').textContent=j.ip||'?';
}).catch(()=>{});
</script>
</body></html>
)HTML";

static String renderIndex() {
  String s(FPSTR(INDEX_HTML));
  s.replace("__SSID__", configPtr->ssid);
  s.replace("__URL__",  configPtr->upload_url);
  s.replace("__DEV__",  configPtr->device_id);
  return s;
}

void portalBegin(Config *cfg) {
  configPtr = cfg;
  saveRequested = false;

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/html", renderIndex());
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    bool isAp = (WiFi.getMode() & WIFI_AP);
    String ip = isAp ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    uint64_t sdFree = 0;
    if (SD.cardType() != CARD_NONE) {
      sdFree = SD.totalBytes() - SD.usedBytes();
    }
    String s = "{";
    s += "\"device_id\":\"" + configPtr->device_id + "\",";
    s += "\"mode\":\"" + String(isAp ? "ap" : "sta") + "\",";
    s += "\"ip\":\""   + ip + "\",";
    s += "\"sd_free_mb\":" + String((uint32_t)(sdFree / 1024 / 1024));
    s += "}";
    req->send(200, "application/json", s);
  });

  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *req) {
    auto getp = [&](const char *k) -> String {
      if (req->hasParam(k, true)) return req->getParam(k, true)->value();
      return "";
    };
    Config c = *configPtr;
    String ssid = getp("ssid");           if (ssid.length()) c.ssid = ssid;
    String psk  = getp("psk");            if (psk.length())  c.psk = psk;
    String url  = getp("upload_url");     if (url.length())  c.upload_url = url;
    String key  = getp("api_key");        if (key.length())  c.api_key = key;
    String dev  = getp("device_id");      if (dev.length())  c.device_id = dev;
    saveConfig(c);
    *configPtr = c;
    req->send(200, "text/plain", "Saved. Rebooting...");
    saveRequested = true;
  });

  server.onNotFound([](AsyncWebServerRequest *req) {
    req->redirect("/");
  });

  server.begin();
}

void portalEnd() {
  server.end();
}

bool portalSaveRequested() { return saveRequested; }
