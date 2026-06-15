#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include "pins.h"
#include "config.h"
#include "button.h"
#include "audio.h"
#include "wav.h"
#include "battery.h"
#include "ble_sync.h"

// Post-WiFi-removal state machine: the phone (over BLE) is the sole uplink.
//   Boot      transient pre-init state
//   Idle      advertising + serving BLE sync; short press -> record
//   Recording capturing audio to SD via two FreeRTOS tasks; BLE not serviced
enum class State { Boot, Idle, Recording };

static State    state = State::Boot;
static Config   cfg;
static File     recFile;
static String   recPath;
static volatile uint32_t recordedBytes = 0;

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

static bool isRecordingName(const String &base) {
  return base.length() && !base.startsWith(".") && base.endsWith(".wav");
}

// Count valid pending WAVs for the advertised PENDING bit/count.
static uint32_t countPending() {
  if (SD.cardType() == CARD_NONE) return 0;
  File dir = SD.open(PENDING_DIR);
  uint32_t count = 0;
  if (dir) {
    while (true) {
      File entry = dir.openNextFile();
      if (!entry) break;
      String n = String(entry.name());
      entry.close();
      int slash = n.lastIndexOf('/');
      String base = slash >= 0 ? n.substring(slash + 1) : n;
      if (isRecordingName(base)) count++;
    }
    dir.close();
  }
  return count;
}

static uint8_t batteryPct() {
#ifdef ENABLE_BATTERY
  BatteryReading b;
  if (batteryRead(b) && b.percent >= 0 && b.percent <= 100) return (uint8_t)b.percent;
#endif
  return 0xFF;  // unknown
}

// Recompute the advertised pending count + battery and re-advertise (sets the
// PENDING flag bit and the fast/slow interval). Main-loop only.
static void updatePendingAdvertising() {
  bleSyncUpdatePending(countPending(), batteryPct());
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
  state = State::Idle;
  Serial.printf("Stopped, queued %s (%u bytes)\n", target.c_str(), (unsigned)total);

  // New pending file: set the PENDING adv bit + fast interval so the phone
  // wakes and drains. (The CRC of the new file is computed lazily on LIST/GET
  // and cached then — acceptable on the small pending queue.)
  updatePendingAdvertising();
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

#ifdef ENABLE_BATTERY
  if (batteryBegin()) {
    BatteryReading b;
    if (batteryRead(b)) Serial.printf("Battery gauge: %d%% (%d mV)\n", b.percent, b.millivolts);
    else                Serial.println("Battery gauge present but read failed");
  } else {
    Serial.println("Battery gauge not detected on I2C (addon build)");
  }
#endif

  loadConfig(cfg);  // device_id only
  Serial.printf("Config: dev=%s\n", cfg.device_id.c_str());

  // BLE is the sole uplink. Sets up the GATT service (CTRL/STAT/DATA/CFG),
  // security (Just-Works bonding), the resolving list, and starts advertising.
  bleSyncBegin(cfg.device_id);
  // Reflect the pending queue in the advertisement at boot.
  updatePendingAdvertising();

  state = State::Idle;
}

void loop() {
  if (buttonLongPressed()) {
    Serial.println("Long press: factory reset");
    clearConfig();
    NimBLEDevice::deleteAllBonds();  // bonds live in a separate NVS namespace
    delay(200);
    ESP.restart();
  }

  if (buttonTriplePressed() && state != State::Recording) {
    Serial.println("Triple press: opening BLE pairing window");
    bleSyncEnterPairing();
  }

  switch (state) {
    case State::Boot:
      delay(20);
      break;
    case State::Idle:
      bleSyncService();  // advances LIST/GET window/ACK/CFG + pairing timer
      if (buttonShortPressed()) {
        startRecording();
      } else {
        delay(5);  // tight loop so the streaming window stays fed
      }
      break;
    case State::Recording:
      // Audio + SD writes happen in background tasks; BLE is intentionally NOT
      // serviced here (no SD contention). A connected phone's in-flight GET
      // times out and resumes after recording stops.
      if (buttonShortPressed()) {
        stopRecording();
      } else {
        delay(20);
      }
      break;
  }
}
