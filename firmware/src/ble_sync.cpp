#include "ble_sync.h"
#include <NimBLEDevice.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Custom GATT contract — see firmware/src/ble_sync.h and the iOS companion app
// (app/ios/PrawndSync) which implements the other half. Tab-delimited text on
// the Status characteristic, raw bytes on Data.
//
//   Phone -> Control (write):
//     "LIST"            enumerate /pending
//     "GET\t<name>"     stream one file
//     "ACK\t<name>"     mark uploaded (move /pending -> /uploaded)
//
//   Device -> Status (notify), one record per notification, '\n'-terminated:
//     "F\t<name>\t<size>\n"   a pending file (repeated)
//     "FEND\t<count>\n"       end of LIST
//     "GS\t<name>\t<size>\n"  start of a GET; <size> bytes follow on Data
//     "GE\t<name>\n"          end of file (all bytes sent)
//     "OK\t<name>\n"          ACK succeeded
//     "ERR\t<reason>\t<name>\n"
//
//   Device -> Data (notify): raw WAV bytes in MTU-sized chunks during a GET.
static const char *SVC_UUID  = "e2c50000-7c6e-4f1d-9b1a-9b6a4f2d8a01";
static const char *CTRL_UUID = "e2c50001-7c6e-4f1d-9b1a-9b6a4f2d8a01";
static const char *STAT_UUID = "e2c50002-7c6e-4f1d-9b1a-9b6a4f2d8a01";
static const char *DATA_UUID = "e2c50003-7c6e-4f1d-9b1a-9b6a4f2d8a01";

// Pairing model: no PIN. The device only accepts NEW bonds while a pairing
// window is open (opened by a triple button press → bleSyncEnterPairing()).
// Pairing itself is "Just Works" (silent, encrypted, no passkey) — the security
// boundary is physical access to the button, like consumer BT devices. Outside
// the window the device still advertises (so bonded phones can reconnect to
// sync) but rejects connections from phones it isn't already bonded with.
static const uint32_t PAIRING_WINDOW_MS = 60000;

// Advertised manufacturer-data flag the app's pair screen filters on: byte 3 is
// 0x01 while the pairing window is open, 0x00 otherwise. Bytes 1-2 are the
// 0xFFFF "no registered company" id (fine for a hobby device).
static volatile bool pairingMode       = false;
static uint32_t      pairingUntil      = 0;
static volatile bool pendingExitPairing = false;

// Mirror of main.cpp's queue dirs. Kept in sync deliberately — BLE and WiFi
// drain the same on-SD queue.
static const char *PENDING_DIR  = "/pending";
static const char *UPLOADED_DIR = "/uploaded";

static NimBLECharacteristic *statChar = nullptr;
static NimBLECharacteristic *dataChar = nullptr;
static String               advName;

// (Re)build and restart advertising with the pairing flag set or cleared. Only
// called from the main loop (bleSyncBegin / bleSyncService), never from a BLE
// callback, so the advertising stack isn't reconfigured underneath itself.
static void refreshAdvertising(bool pairing);

// A connected phone writes one command at a time on the host task; the main
// loop drains it. Guarded by a mutex so the String copy is consistent.
static SemaphoreHandle_t cmdMutex   = nullptr;
static String            pendingCmd;
static volatile bool     hasCmd     = false;
static volatile bool     connected  = false;
static volatile uint16_t connMtu    = 23;  // default ATT MTU until negotiated

static void notifyStatus(const String &line) {
  if (!statChar) return;
  statChar->setValue((const uint8_t *)line.c_str(), line.length());
  statChar->notify();
  delay(6);  // let the controller drain its notify buffers
}

class ControlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    NimBLEAttValue v = c->getValue();
    if (!v.length()) return;
    if (xSemaphoreTake(cmdMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      pendingCmd = String((const char *)v.data(), v.length());
      hasCmd = true;
      xSemaphoreGive(cmdMutex);
    }
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, NimBLEConnInfo &info) override {
    // Only already-bonded phones may connect outside the pairing window. This is
    // what makes pairing a deliberate, button-gated act rather than open access.
    if (!pairingMode && !info.isBonded()) {
      Serial.println("BLE: rejecting unpaired phone (not in pairing mode)");
      s->disconnect(info.getConnHandle());
      return;
    }
    connected = true;
    connMtu = info.getMTU();
    Serial.printf("BLE phone connected (MTU %u, bonded=%d)\n", connMtu, info.isBonded());
  }
  void onDisconnect(NimBLEServer *s, NimBLEConnInfo &, int) override {
    connected = false;
    connMtu = 23;
    Serial.println("BLE phone disconnected");
    NimBLEDevice::startAdvertising();  // re-advertise for the next sync
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo &) override { connMtu = mtu; }

  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    if (info.isEncrypted()) {
      Serial.println("BLE link encrypted (bonded)");
      // One phone paired — close the window (handled in the main loop so we
      // don't reconfigure advertising from inside a BLE callback).
      if (pairingMode) pendingExitPairing = true;
    } else {
      Serial.println("BLE pairing FAILED");
    }
  }
};

void bleSyncBegin(const String &deviceId) {
  cmdMutex = xSemaphoreCreateMutex();

  NimBLEDevice::init(deviceId.c_str());
  NimBLEDevice::setMTU(517);  // request a large ATT MTU; iOS negotiates down

  // Bonding + LE Secure Connections, no MITM → "Just Works" (no passkey). New
  // bonds are gated to the pairing window in ServerCallbacks::onConnect.
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService *svc = server->createService(SVC_UUID);
  // WRITE_ENC requires an encrypted link (i.e. a bond) before any command is
  // accepted, so the audio notifications that follow are encrypted too.
  svc->createCharacteristic(CTRL_UUID,
                            NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR |
                            NIMBLE_PROPERTY::WRITE_ENC)
      ->setCallbacks(new ControlCallbacks());
  statChar = svc->createCharacteristic(STAT_UUID, NIMBLE_PROPERTY::NOTIFY);
  dataChar = svc->createCharacteristic(DATA_UUID, NIMBLE_PROPERTY::NOTIFY);
  svc->start();

  advName = deviceId;
  refreshAdvertising(false);
  Serial.printf("BLE advertising as %s\n", deviceId.c_str());
}

static void refreshAdvertising(bool pairing) {
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->stop();

  // Main packet: flags (3) + 128-bit service UUID (18) + manufacturer data
  // (5) = 26 bytes, fits the 31-byte limit. Name goes in the scan response.
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.addServiceUUID(SVC_UUID);
  uint8_t mfg[3] = {0xFF, 0xFF, (uint8_t)(pairing ? 0x01 : 0x00)};
  advData.setManufacturerData(mfg, sizeof(mfg));
  adv->setAdvertisementData(advData);

  NimBLEAdvertisementData scanResp;
  scanResp.setName(advName.c_str());
  adv->setScanResponseData(scanResp);

  adv->start();
}

static void exitPairing() {
  if (!pairingMode) return;
  pairingMode = false;
  pendingExitPairing = false;
  refreshAdvertising(false);
  Serial.println("BLE pairing window closed");
}

void bleSyncEnterPairing() {
  pairingMode  = true;
  pairingUntil = millis() + PAIRING_WINDOW_MS;
  pendingExitPairing = false;
  refreshAdvertising(true);
  Serial.println("BLE pairing window OPEN (60s, no PIN)");
}

bool bleSyncPairing() { return pairingMode; }

bool bleSyncBusy() { return connected; }

// True for a valid recording filename — skips macOS AppleDouble sidecars and
// anything that isn't one of our WAVs, same guard as the WiFi drain path.
static bool isRecordingName(const String &base) {
  return base.length() && !base.startsWith(".") && base.endsWith(".wav");
}

static void handleList() {
  File dir = SD.open(PENDING_DIR);
  uint32_t count = 0;
  if (dir) {
    while (true) {
      File entry = dir.openNextFile();
      if (!entry) break;
      String n = String(entry.name());
      uint32_t sz = entry.size();
      entry.close();
      int slash = n.lastIndexOf('/');
      String base = slash >= 0 ? n.substring(slash + 1) : n;
      if (!isRecordingName(base)) continue;
      notifyStatus(String("F\t") + base + "\t" + sz + "\n");
      count++;
    }
    dir.close();
  }
  notifyStatus(String("FEND\t") + count + "\n");
}

static void handleGet(const String &name) {
  String path = String(PENDING_DIR) + "/" + name;
  File f = SD.open(path, FILE_READ);
  if (!f) {
    notifyStatus(String("ERR\tnotfound\t") + name + "\n");
    return;
  }
  uint32_t size = f.size();
  notifyStatus(String("GS\t") + name + "\t" + size + "\n");

  // Chunk to the negotiated MTU minus the 3-byte ATT notification header.
  size_t chunk = connMtu > 23 ? connMtu - 3 : 20;
  if (chunk > 512) chunk = 512;
  static uint8_t buf[512];
  while (connected) {
    size_t n = f.read(buf, chunk);
    if (n == 0) break;
    dataChar->setValue(buf, n);
    dataChar->notify();
    delay(6);  // pace so iOS / the controller can keep up
  }
  f.close();
  notifyStatus(String("GE\t") + name + "\n");
}

static void handleAck(const String &name) {
  String src = String(PENDING_DIR) + "/" + name;
  String dst = String(UPLOADED_DIR) + "/" + name;
  if (!SD.exists(src)) {
    notifyStatus(String("ERR\tnotfound\t") + name + "\n");
    return;
  }
  if (SD.exists(dst)) SD.remove(dst);
  if (SD.rename(src, dst)) {
    notifyStatus(String("OK\t") + name + "\n");
    Serial.printf("BLE ack: %s -> uploaded\n", name.c_str());
  } else {
    notifyStatus(String("ERR\trename\t") + name + "\n");
  }
}

void bleSyncService() {
  // Close the pairing window on timeout, or once a phone has bonded.
  if (pairingMode &&
      (pendingExitPairing || (int32_t)(millis() - pairingUntil) >= 0)) {
    exitPairing();
  }

  if (!hasCmd) return;
  String cmd;
  if (xSemaphoreTake(cmdMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    cmd = pendingCmd;
    hasCmd = false;
    xSemaphoreGive(cmdMutex);
  } else {
    return;
  }

  int tab = cmd.indexOf('\t');
  String verb = tab >= 0 ? cmd.substring(0, tab) : cmd;
  String arg  = tab >= 0 ? cmd.substring(tab + 1) : String();
  arg.trim();
  verb.trim();

  if (SD.cardType() == CARD_NONE) {
    notifyStatus("ERR\tnosd\t\n");
    return;
  }
  if (verb == "LIST")      handleList();
  else if (verb == "GET")  handleGet(arg);
  else if (verb == "ACK")  handleAck(arg);
  else                     notifyStatus(String("ERR\tbadcmd\t") + verb + "\n");
}
