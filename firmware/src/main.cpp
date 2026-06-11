#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include "pins.h"
#include "config.h"
#include "wifi_mgr.h"
#include "portal.h"
#include "button.h"
#include "audio.h"
#include "wav.h"
#include "uploader.h"

enum class State { Boot, ApPortal, StaIdle, Recording, Uploading };

static State    state = State::Boot;
static Config   cfg;
static File     recFile;
static String   recPath;
static volatile uint32_t recordedBytes = 0;
static String   pendingUploadPath;
static uint32_t lastDrainAt = 0;
static bool     firstDrain  = true;

// --- Recording pipeline (FreeRTOS) ----------------------------------------
// audioReaderTask: blocks on I2S, applies filters, pushes int16 frames into a
// 64 KB stream buffer. sdWriterTask: drains the buffer in 4 KB chunks and
// writes to the open WAV file. Decouples the audio sampling cadence from SD
// write bursts — eliminates the 62.5 Hz envelope modulation the old single-
// threaded loop produced.
static StreamBufferHandle_t recStream  = nullptr;
static volatile bool        recStop    = false;
static volatile bool        readerDone = false;
static volatile bool        writerDone = false;

static void audioReaderTask(void *) {
  static int16_t out[256];
  while (!recStop) {
    size_t n = audioReadFrames(out, 256);
    if (n) {
      xStreamBufferSend(recStream, out, n * sizeof(int16_t), portMAX_DELAY);
    }
  }
  readerDone = true;
  vTaskDelete(NULL);
}

static void sdWriterTask(void *) {
  static uint8_t buf[4096];
  while (true) {
    size_t got = xStreamBufferReceive(recStream, buf, sizeof(buf), pdMS_TO_TICKS(100));
    if (got > 0) {
      recordedBytes += recFile.write(buf, got);
    } else if (readerDone && xStreamBufferIsEmpty(recStream)) {
      break;
    }
  }
  writerDone = true;
  vTaskDelete(NULL);
}

static const String PENDING_DIR  = "/pending";
static const String UPLOADED_DIR = "/uploaded";

static void ensureDirs() {
  if (!SD.exists(PENDING_DIR))  SD.mkdir(PENDING_DIR);
  if (!SD.exists(UPLOADED_DIR)) SD.mkdir(UPLOADED_DIR);
}

static String findFirstPending() {
  File dir = SD.open(PENDING_DIR);
  if (!dir) return String();
  String name;
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;
    String n = String(entry.name());
    entry.close();
    // Skip macOS AppleDouble sidecars (._foo.wav) and anything that doesn't
    // look like one of our recordings. Without this guard the device retries
    // ._<file>.wav forever and the server replies 400 every time.
    int slash = n.lastIndexOf('/');
    String base = slash >= 0 ? n.substring(slash + 1) : n;
    if (!base.length() || base.startsWith(".") || !base.endsWith(".wav")) continue;
    name = n;
    break;
  }
  dir.close();
  if (!name.length()) return String();
  if (name.startsWith("/")) return name;
  return PENDING_DIR + "/" + name;
}

static void startRecording() {
  if (SD.cardType() == CARD_NONE) {
    Serial.println("No SD card; cannot record");
    return;
  }
  recPath = String("/rec_") + String(millis()) + ".wav";
  recFile = SD.open(recPath, FILE_WRITE);
  if (!recFile) {
    Serial.printf("Failed to open %s\n", recPath.c_str());
    return;
  }
  wavWritePlaceholderHeader(recFile, 16000, 2, 16);
  recordedBytes = 44;
  if (!audioBegin()) {
    Serial.println("audioBegin failed");
    recFile.close();
    SD.remove(recPath);
    return;
  }
  audioDropSettle();

  // Spin up the two-task recording pipeline.
  recStream  = xStreamBufferCreate(64 * 1024, 1024);
  recStop    = false;
  readerDone = false;
  writerDone = false;
  xTaskCreate(audioReaderTask, "audio_rdr", 4096, NULL, 10, NULL);
  xTaskCreate(sdWriterTask,    "sd_wr",     4096, NULL, 5,  NULL);

  state = State::Recording;
  Serial.printf("Recording -> %s\n", recPath.c_str());
}

static void stopRecording() {
  // Signal reader → it exits the next loop iteration. Writer drains then
  // exits. Wait for both with generous timeouts.
  recStop = true;
  uint32_t t0 = millis();
  while (!readerDone && millis() - t0 < 5000) delay(10);
  while (!writerDone && millis() - t0 < 10000) delay(10);
  audioEnd();
  if (recStream) { vStreamBufferDelete(recStream); recStream = nullptr; }

  uint32_t total = recordedBytes;
  recFile.close();
  if (!wavPatchSizes(recPath.c_str(), total)) {
    Serial.printf("WAV header patch failed for %s\n", recPath.c_str());
  }
  String target = PENDING_DIR + "/" + String(millis()) + ".wav";
  SD.rename(recPath, target);
  pendingUploadPath = target;
  state = State::Uploading;
  Serial.printf("Stopped, queued %s (%u bytes)\n", target.c_str(), (unsigned)total);
}

static void tryUploadOnce() {
  if (!pendingUploadPath.length()) {
    pendingUploadPath = findFirstPending();
  }
  if (!pendingUploadPath.length()) {
    state = State::StaIdle;
    return;
  }
  int code = uploadFile(cfg, pendingUploadPath);
  Serial.printf("Upload %s -> %d\n", pendingUploadPath.c_str(), code);
  if (code >= 200 && code < 300) {
    int slash = pendingUploadPath.lastIndexOf('/');
    String name = pendingUploadPath.substring(slash + 1);
    String dest = UPLOADED_DIR + "/" + name;
    if (SD.exists(dest)) SD.remove(dest);
    SD.rename(pendingUploadPath, dest);
    pendingUploadPath = "";
  } else {
    // Leave the file in /pending for the next retry tick.
    pendingUploadPath = "";
  }
  state = State::StaIdle;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nPrawnd boot");

  buttonBegin();

  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  // 16 kHz × 2ch × 2B = 64 KB/s of audio data — at 400 kHz SPI the card
  // tops out around 50 KB/s and DMA overruns produce robotic-sounding
  // recordings. 1 MHz gives ~125 KB/s, comfortable headroom.
  if (!SD.begin(PIN_SD_CS, SPI, 1000000)) {
    Serial.println("SD init failed");
  } else {
    Serial.printf("SD ok: %u MB total, %u MB free\n",
                  (uint32_t)(SD.totalBytes() / 1024 / 1024),
                  (uint32_t)((SD.totalBytes() - SD.usedBytes()) / 1024 / 1024));
    ensureDirs();
  }

  bool ok = loadConfig(cfg);
  Serial.printf("Config: ssid=%s url=%s dev=%s\n",
                cfg.ssid.c_str(), cfg.upload_url.c_str(), cfg.device_id.c_str());

  if (ok && wifiTryStation(cfg.ssid, cfg.psk)) {
    Serial.printf("STA up, IP %s\n", wifiCurrentIp().c_str());
    portalBegin(&cfg);
    state = State::StaIdle;
  } else {
    Serial.println("Starting AP captive portal");
    wifiStartAp(cfg.device_id);
    portalBegin(&cfg);
    state = State::ApPortal;
  }
}

void loop() {
  if (buttonLongPressed()) {
    Serial.println("Long press: factory reset");
    clearConfig();
    delay(200);
    ESP.restart();
  }

  if (portalSaveRequested()) {
    Serial.println("Config saved; rebooting");
    delay(1000);
    ESP.restart();
  }

  switch (state) {
    case State::Boot:
      delay(20);
      break;
    case State::ApPortal:
      // Just service portal; ignore button until WiFi is configured.
      delay(20);
      break;
    case State::StaIdle:
      if (buttonShortPressed()) {
        startRecording();
      } else if (firstDrain || (millis() - lastDrainAt > 60000)) {
        firstDrain = false;
        lastDrainAt = millis();
        state = State::Uploading;
      } else {
        delay(20);
      }
      break;
    case State::Recording:
      // Audio + SD writes happen in background tasks; main loop just waits
      // for the user to press the button again.
      if (buttonShortPressed()) {
        stopRecording();
      } else {
        delay(20);
      }
      break;
    case State::Uploading:
      tryUploadOnce();
      break;
  }
}
