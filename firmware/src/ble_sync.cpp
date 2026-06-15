#include "ble_sync.h"
#include "config.h"
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ===========================================================================
// Prawnd BLE sync — implements docs/ble-autosync-spec.md Section B (the binding
// wire contract) and Section C (firmware spec). The phone is the sole uplink:
// it pulls /pending/*.wav over a reliable, windowed, CRC'd file transfer and
// ACKs each file (moving it /pending -> /uploaded).
//
//   CTRL (phone -> device, WriteEnc): text commands, one per write —
//     "LIST"                 enumerate /pending  -> STAT "F ...", "FEND <n>"
//     "GET\t<name>\t<off>"   begin/resume sending <name> from byte <off>
//     "ACK\t<name>"          mark uploaded (/pending -> /uploaded)
//     "ABORT\t<name>"        cancel an in-progress GET
//     "WIN\t<n>"             set window size (1..32, default 8)
//     plus a BINARY windowed-ACK frame, distinguished by first byte 0x01:
//       byte[0]=0x01, byte[1..4]=next_seq (u32 LE), byte[5]=flags (bit0=NAK)
//
//   STAT (device -> phone, Notify): tab-delimited, '\n'-terminated text —
//     "F\t<name>\t<size>\t<crc32>\n"        a pending file
//     "FEND\t<count>\n"                     end of LIST
//     "GS\t<name>\t<size>\t<crc32>\t<mtu>\t<chunk>\n"   start of GET
//     "GE\t<name>\t<crc_ok>\n"             end of file
//     "GA\t<name>\n"                        GET aborted
//     "OK\t<name>\n"                        ACK ok
//     "ERR\t<code>\t<name>\n"               error (see B.4.5)
//
//   DATA (device -> phone, Notify): binary frames, one per notification —
//     byte[0..3]=seq (u32 LE), byte[4..5]=len (u16 LE), byte[6..]=payload
//
//   CFG (device <-> phone, Read/WriteEnc): small UTF-8 JSON (B.7).
// ===========================================================================

static const char *SVC_UUID  = "e2c50000-7c6e-4f1d-9b1a-9b6a4f2d8a01";
static const char *CTRL_UUID = "e2c50001-7c6e-4f1d-9b1a-9b6a4f2d8a01";
static const char *STAT_UUID = "e2c50002-7c6e-4f1d-9b1a-9b6a4f2d8a01";
static const char *DATA_UUID = "e2c50003-7c6e-4f1d-9b1a-9b6a4f2d8a01";
static const char *CFG_UUID  = "e2c50004-7c6e-4f1d-9b1a-9b6a4f2d8a01";

static const char *FW_VERSION = "2.0.0";
static const uint8_t PROTO_VER = 0x02;

// Advertising flag bits (mfg-data byte[1], spec B.2).
static const uint8_t ADV_FLAG_PAIRING = 0x01;
static const uint8_t ADV_FLAG_PENDING = 0x02;
static const uint8_t ADV_FLAG_BUSY    = 0x04;

// Advertising intervals in 0.625 ms units (spec B.2).
//   idle (pending=0): 1000–1285 ms ; fast (pending or pairing): 152.5–211.25 ms
static const uint16_t ADV_ITVL_SLOW_MIN = 1600;  // 1000 ms
static const uint16_t ADV_ITVL_SLOW_MAX = 2056;  // 1285 ms
static const uint16_t ADV_ITVL_FAST_MIN = 244;   // 152.5 ms
static const uint16_t ADV_ITVL_FAST_MAX = 338;   // 211.25 ms

static const uint32_t PAIRING_WINDOW_MS = 60000;

static const char *PENDING_DIR  = "/pending";
static const char *UPLOADED_DIR = "/uploaded";

// ----- GATT objects --------------------------------------------------------
static NimBLECharacteristic *ctrlChar = nullptr;
static NimBLECharacteristic *statChar = nullptr;
static NimBLECharacteristic *dataChar = nullptr;
static NimBLECharacteristic *cfgChar  = nullptr;
static String               advName;

// ----- pairing window ------------------------------------------------------
static volatile bool pairingMode        = false;
static uint32_t      pairingUntil       = 0;
static volatile bool pendingExitPairing = false;

// ----- connection state ----------------------------------------------------
static volatile bool     connected  = false;
static volatile uint16_t connMtu    = 23;          // ATT MTU until negotiated
static volatile uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;

// ----- advertised state (recomputed by bleSyncUpdatePending) ---------------
static uint32_t advPendingCount = 0;
static uint8_t  advBatteryPct   = 0xFF;

// ----- new-bond enforcement (B.3) ------------------------------------------
// Snapshot of getNumBonds() taken whenever the pairing window opens (and at
// boot). A bond formed while the window is closed grows this count; we then
// delete that bond and disconnect on the main loop.
static int            bondsBeforeAuth = 0;
static volatile bool  hasRejectPeer   = false;
static NimBLEAddress  rejectPeer;
static volatile uint16_t rejectHandle = BLE_HS_CONN_HANDLE_NONE;

// ----- command/ACK enqueue (callbacks enqueue; main loop services) ---------
static SemaphoreHandle_t cmdMutex = nullptr;
static String            pendingCmd;
static volatile bool     hasCmd = false;
// Latest windowed-ACK from the phone (last-write-wins; cumulative ACK so an
// older one is harmlessly superseded).
static volatile bool     hasAckw      = false;
static volatile uint32_t ackwNextSeq  = 0;
static volatile bool     ackwNak      = false;
// Pending device-id change from a CFG write (applied on the main loop).
static volatile bool     hasCfgChange = false;
static String            cfgNewId;

// ----- forward decls -------------------------------------------------------
static void refreshAdvertising();
static void notifyStatus(const String &line);

// ===========================================================================
// CRC-32 (IEEE 802.3, poly 0xEDB88320, reflected, init/final 0xFFFFFFFF) — B.4.4
// ===========================================================================
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
    }
  }
  return ~crc;
}

// ----- CRC cache (C.6): filename+size -> crc, recomputed on miss -----------
struct CrcEntry { String name; uint32_t size; uint32_t crc; bool used; };
static const int CRC_CACHE_N = 16;
static CrcEntry crcCache[CRC_CACHE_N];

static bool crcCacheLookup(const String &name, uint32_t size, uint32_t &crc) {
  for (int i = 0; i < CRC_CACHE_N; i++) {
    if (crcCache[i].used && crcCache[i].size == size && crcCache[i].name == name) {
      crc = crcCache[i].crc;
      return true;
    }
  }
  return false;
}

static void crcCacheInsert(const String &name, uint32_t size, uint32_t crc) {
  // Replace an existing same-name entry, else the first free slot, else evict 0.
  int slot = -1;
  for (int i = 0; i < CRC_CACHE_N; i++) {
    if (crcCache[i].used && crcCache[i].name == name) { slot = i; break; }
  }
  if (slot < 0) for (int i = 0; i < CRC_CACHE_N; i++) {
    if (!crcCache[i].used) { slot = i; break; }
  }
  if (slot < 0) slot = 0;
  crcCache[slot].name = name;
  crcCache[slot].size = size;
  crcCache[slot].crc  = crc;
  crcCache[slot].used = true;
}

// Compute a file's CRC-32 by streaming it in 4 KB reads. Returns false on
// read error. Caches the result. This is the only place that reads a whole
// file off SD; it runs on the main loop (LIST miss / stopRecording).
static bool fileCrc32(const String &path, const String &name, uint32_t size,
                      uint32_t &outCrc) {
  if (crcCacheLookup(name, size, outCrc)) return true;
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  static uint8_t rbuf[4096];
  // crc32_update() does its own ~crc at start/end, so the chaining seed and the
  // result are the "finalised" (non-inverted) running value; seed with 0.
  uint32_t crc = 0;
  while (true) {
    size_t n = f.read(rbuf, sizeof(rbuf));
    if (n == 0) break;
    crc = crc32_update(crc, rbuf, n);
  }
  f.close();
  outCrc = crc;
  crcCacheInsert(name, size, crc);
  return true;
}

static String crcHex(uint32_t crc) {
  char buf[9];
  snprintf(buf, sizeof(buf), "%08x", crc);
  return String(buf);
}

// ===========================================================================
// helpers
// ===========================================================================
static bool isRecordingName(const String &base) {
  return base.length() && !base.startsWith(".") && base.endsWith(".wav");
}

// Validate a GET/ACK name: no path separators, not dot-prefixed, ends .wav.
static bool validName(const String &name) {
  if (!isRecordingName(name)) return false;
  if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) return false;
  return true;
}

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

// STAT notify. Text status lines are short (< MTU); no pacing delay — the
// controller back-pressures via the notify() return, but for the handful of
// STAT lines per command we just fire them.
static void notifyStatus(const String &line) {
  if (!statChar) return;
  statChar->notify((const uint8_t *)line.c_str(), line.length(), connHandle);
}

// ===========================================================================
// GET sender state machine (B.5)
// ===========================================================================
enum class GetState { Idle, Streaming, Draining };
static GetState getState   = GetState::Idle;
static File     getFile;
static String   getName;
static uint32_t getFileSize    = 0;
static uint32_t getFileCrc     = 0;
static uint32_t getChunk       = 0;   // payload bytes per frame
static uint32_t getTotalFrames = 0;
static uint32_t getNextSeq     = 0;   // next frame index to send
static uint32_t getWinLo       = 0;   // lowest seq not yet cumulatively ACKed
static uint8_t  getWindow      = 8;   // W (1..32), set via WIN
static uint32_t getLastProgress = 0;  // millis() of last ACKW progress

static void getReset() {
  if (getFile) getFile.close();
  getState = GetState::Idle;
  getName  = "";
}

// Compute payload chunk size from the negotiated MTU: (MTU - 3) - 6 header.
static uint32_t computeChunk() {
  uint32_t att = connMtu > 23 ? (uint32_t)connMtu - 3 : 20;  // ATT payload
  uint32_t c = att > 6 ? att - 6 : 1;                        // minus DATA header
  if (c > 506) c = 506;  // 517-3-6 ceiling; bound the static staging buffer
  if (c < 1) c = 1;
  return c;
}

static void handleGet(const String &name, uint32_t offset) {
  if (getState != GetState::Idle) {
    notifyStatus(String("ERR\tbusy\t") + name + "\n");
    return;
  }
  if (!validName(name)) {
    notifyStatus(String("ERR\tbadname\t") + name + "\n");
    return;
  }
  String path = String(PENDING_DIR) + "/" + name;
  if (!SD.exists(path)) {
    notifyStatus(String("ERR\tnotfound\t") + name + "\n");
    return;
  }
  File f = SD.open(path, FILE_READ);
  if (!f) {
    notifyStatus(String("ERR\tinternal\t") + name + "\n");
    return;
  }
  uint32_t size = f.size();
  if (offset > size) {
    f.close();
    notifyStatus(String("ERR\tbadoffset\t") + name + "\n");
    return;
  }
  uint32_t crc = 0;
  if (!fileCrc32(path, name, size, crc)) {
    f.close();
    notifyStatus(String("ERR\tinternal\t") + name + "\n");
    return;
  }
  uint32_t chunk = computeChunk();
  // Frame seq is offset/chunk. Spec keeps all-but-last frame exactly `chunk`
  // bytes, so resume offsets are chunk-aligned; clamp to a chunk boundary.
  uint32_t base = offset / chunk;
  uint32_t seekTo = base * chunk;
  if (!f.seek(seekTo)) {
    f.close();
    notifyStatus(String("ERR\tinternal\t") + name + "\n");
    return;
  }

  getFile        = f;
  getName        = name;
  getFileSize    = size;
  getFileCrc     = crc;
  getChunk       = chunk;
  getTotalFrames = size == 0 ? 0 : (size + chunk - 1) / chunk;
  getNextSeq     = base;
  getWinLo       = base;
  getLastProgress = millis();
  getState       = GetState::Streaming;

  // GS\t<name>\t<size>\t<crc32>\t<mtu>\t<chunk>
  notifyStatus(String("GS\t") + name + "\t" + size + "\t" + crcHex(crc) + "\t" +
               connMtu + "\t" + chunk + "\n");
}

// Pump frames within the window. Cooperative — returns after a bounded burst so
// the main loop stays responsive. Called every bleSyncService() tick.
static void getPump() {
  if (getState != GetState::Streaming) return;
  static uint8_t frame[6 + 506];
  // Bound the work per tick so the loop never blocks long; the window (W<=32)
  // already bounds in-flight frames, but cap iterations defensively.
  int budget = getWindow > 0 ? getWindow : 8;
  while (connected && getNextSeq < getWinLo + getWindow &&
         getNextSeq < getTotalFrames && budget-- > 0) {
    uint32_t off = getNextSeq * getChunk;
    if (!getFile.seek(off)) { getReset(); return; }
    size_t n = getFile.read(frame + 6, getChunk);
    if (n == 0) break;
    frame[0] = (uint8_t)(getNextSeq);
    frame[1] = (uint8_t)(getNextSeq >> 8);
    frame[2] = (uint8_t)(getNextSeq >> 16);
    frame[3] = (uint8_t)(getNextSeq >> 24);
    frame[4] = (uint8_t)(n);
    frame[5] = (uint8_t)(n >> 8);
    if (!dataChar->notify(frame, 6 + n, connHandle)) {
      // Controller TX queue full — back off and retry next tick (no delay()).
      break;
    }
    getNextSeq++;
  }
  // EOF: all frames have been sent at least once → wait for cumulative ACKs.
  if (getNextSeq >= getTotalFrames) {
    getState = GetState::Draining;
  }
}

static void getDrain() {
  if (getState != GetState::Draining) return;
  if (getWinLo >= getTotalFrames) {
    // All frames cumulatively ACKed. crc_ok=1: file on disk is intact/closed;
    // authoritative verification is the app's (B.5 note).
    notifyStatus(String("GE\t") + getName + "\t1\n");
    getReset();
  }
}

static void applyAckw(uint32_t nextAck, bool nak) {
  if (getState != GetState::Streaming && getState != GetState::Draining) return;
  if (nak) {
    // Rewind: resend from nextAck.
    getNextSeq = nextAck;
    if (getNextSeq < getWinLo) getWinLo = getNextSeq;
    if (getState == GetState::Draining) getState = GetState::Streaming;
  }
  if (nextAck > getWinLo) {
    getWinLo = nextAck;          // slide the window forward
  }
  getLastProgress = millis();
}

static void handleAbort(const String &name) {
  if (getState != GetState::Idle && getName == name) {
    getReset();
  }
  notifyStatus(String("GA\t") + name + "\n");
}

// ===========================================================================
// LIST / ACK
// ===========================================================================
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
      uint32_t crc = 0;
      String path = String(PENDING_DIR) + "/" + base;
      if (!fileCrc32(path, base, sz, crc)) {
        notifyStatus(String("ERR\tinternal\t") + base + "\n");
        continue;
      }
      notifyStatus(String("F\t") + base + "\t" + sz + "\t" + crcHex(crc) + "\n");
      count++;
    }
    dir.close();
  }
  notifyStatus(String("FEND\t") + count + "\n");
}

static void handleAck(const String &name) {
  if (!validName(name)) {
    notifyStatus(String("ERR\tbadname\t") + name + "\n");
    return;
  }
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
    // Queue shrank — refresh advertised PENDING bit/count.
    bleSyncUpdatePending(countPending(), advBatteryPct);
  } else {
    notifyStatus(String("ERR\trename\t") + name + "\n");
  }
}

// ===========================================================================
// CFG (B.7)
// ===========================================================================
static void buildCfgJson(String &out) {
  JsonDocument doc;
  doc["device_id"] = advName;
  doc["fw"]        = FW_VERSION;
  doc["proto"]     = PROTO_VER;
  uint32_t freeMb = 0;
  if (SD.cardType() != CARD_NONE) {
    freeMb = (uint32_t)((SD.totalBytes() - SD.usedBytes()) / (1024 * 1024));
  }
  doc["free_mb"]   = freeMb;
  doc["pending"]   = advPendingCount;
  serializeJson(doc, out);
}

static bool validDeviceId(const String &id) {
  if (id.length() < 1 || id.length() > 32) return false;
  for (size_t i = 0; i < id.length(); i++) {
    char c = id[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

// ===========================================================================
// Callbacks (BLE host task) — only enqueue; main loop does the work
// ===========================================================================
class ControlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    NimBLEAttValue v = c->getValue();
    if (!v.length()) return;
    const uint8_t *d = v.data();
    // Binary windowed-ACK is distinguished by first byte 0x01 (B.4.1).
    if (d[0] == 0x01 && v.length() >= 5) {
      uint32_t next = (uint32_t)d[1] | ((uint32_t)d[2] << 8) |
                      ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 24);
      bool nak = v.length() >= 6 ? (d[5] & 0x01) : false;
      ackwNextSeq = next;
      ackwNak     = nak;
      hasAckw     = true;
      return;
    }
    // Text command — last-write-wins (one command at a time per contract).
    if (xSemaphoreTake(cmdMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      pendingCmd = String((const char *)d, v.length());
      hasCmd = true;
      xSemaphoreGive(cmdMutex);
    }
  }
};

class CfgCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    String json;
    buildCfgJson(json);
    c->setValue((const uint8_t *)json.c_str(), json.length());
  }
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    NimBLEAttValue v = c->getValue();
    if (!v.length()) return;
    JsonDocument doc;
    if (deserializeJson(doc, v.data(), v.length()) != DeserializationError::Ok) return;
    if (doc["device_id"].is<const char *>()) {
      String id = String((const char *)doc["device_id"]);
      if (validDeviceId(id)) {
        // Defer persist + re-advertise to the main loop (avoid NVS/adv churn
        // in a BLE callback). saveDeviceId() is done there.
        cfgNewId = id;
        hasCfgChange = true;
      }
    }
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, NimBLEConnInfo &info) override {
    // Bug #1 fix: do NOT gate on info.isBonded() — a not-yet-resolved iOS RPA
    // reads as unbonded at connect time. Access is enforced at the GATT layer
    // (CTRL/CFG are WriteEnc/ReadEnc), so an unbonded/unencrypted central
    // simply cannot issue commands; new-bond creation is gated to the pairing
    // window in onAuthenticationComplete. We accept every connection here.
    connected  = true;
    connMtu    = info.getMTU();
    connHandle = info.getConnHandle();
    Serial.printf("BLE phone connected (MTU %u, bonded=%d)\n",
                  connMtu, info.isBonded());
  }
  void onDisconnect(NimBLEServer *s, NimBLEConnInfo &, int) override {
    connected  = false;
    connMtu    = 23;
    connHandle = BLE_HS_CONN_HANDLE_NONE;
    getReset();                  // abort any in-flight GET; /pending intact
    Serial.println("BLE phone disconnected");
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo &) override { connMtu = mtu; }

  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    if (!info.isEncrypted()) {
      Serial.println("BLE pairing FAILED");
      return;
    }
    Serial.println("BLE link encrypted");
    if (pairingMode) {
      // A new bond is welcome during the window — close it (on the main loop).
      pendingExitPairing = true;
      return;
    }
    // Window CLOSED. New-bond gate (B.3 / C.5): if SMP just created a brand-new
    // bond (bond count grew beyond the snapshot taken at last reset/open), this
    // is an unauthorised pairing — flag the peer for delete+disconnect on the
    // main loop. A reconnect of an existing bond re-encrypts via the stored LTK
    // without growing getNumBonds(), so it is left alone.
    if (NimBLEDevice::getNumBonds() > bondsBeforeAuth) {
      rejectPeer = info.getIdAddress();
      rejectHandle = info.getConnHandle();
      hasRejectPeer = true;
    }
  }
};

void bleSyncBegin(const String &deviceId) {
  cmdMutex = xSemaphoreCreateMutex();

  NimBLEDevice::init(deviceId.c_str());
  NimBLEDevice::setMTU(517);  // request a large ATT MTU; iOS negotiates down

  // Just-Works bonding + LE Secure Connections (B.3). Bonds persist in NimBLE's
  // own NVS namespace across reboot; we never wipe ble_store except on factory
  // reset (deleteAllBonds(), in main.cpp's long-press path).
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService *svc = server->createService(SVC_UUID);
  // CTRL is WRITE_ENC: an unencrypted (unbonded) central gets Insufficient
  // Authentication on any command write, and iOS then auto-initiates pairing.
  ctrlChar = svc->createCharacteristic(CTRL_UUID,
                 NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR |
                 NIMBLE_PROPERTY::WRITE_ENC);
  ctrlChar->setCallbacks(new ControlCallbacks());

  // STAT/DATA notify. STAT is also ReadEnc per the contract.
  statChar = svc->createCharacteristic(STAT_UUID,
                 NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC);
  dataChar = svc->createCharacteristic(DATA_UUID, NIMBLE_PROPERTY::NOTIFY);

  cfgChar = svc->createCharacteristic(CFG_UUID,
                 NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC |
                 NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  cfgChar->setCallbacks(new CfgCallbacks());

  svc->start();

  advName = deviceId;
  bondsBeforeAuth = NimBLEDevice::getNumBonds();  // baseline for new-bond gate
  refreshAdvertising();
  Serial.printf("BLE advertising as %s (%d bonds)\n",
                deviceId.c_str(), NimBLEDevice::getNumBonds());
}

// Build the B.2 advertising payload and (re)start advertising at the interval
// implied by the current flags. Main-loop only.
static void refreshAdvertising() {
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->stop();

  uint8_t flags = 0;
  if (pairingMode)          flags |= ADV_FLAG_PAIRING;
  if (advPendingCount > 0)  flags |= ADV_FLAG_PENDING;
  if (connected && getState != GetState::Idle) flags |= ADV_FLAG_BUSY;

  // Flags(3) + 128-bit SVC UUID(18) + mfg-data(2 company + 4 payload + 2 AD
  // header = effectively 7 bytes counting the company id within the 9-byte AD
  // structure) = 30 bytes; fits 31. SVC UUID is in the PRIMARY packet so iOS
  // background scan (which honours only the primary packet) can match.
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.addServiceUUID(SVC_UUID);
  // mfg payload after the 0xFFFF company id: {ver, flags, count, batt}.
  uint8_t mfg[6];
  mfg[0] = 0xFF;  // company id LSB (0xFFFF = "no registered company")
  mfg[1] = 0xFF;  // company id MSB
  mfg[2] = PROTO_VER;
  mfg[3] = flags;
  mfg[4] = (uint8_t)(advPendingCount > 255 ? 255 : advPendingCount);
  mfg[5] = advBatteryPct;
  advData.setManufacturerData(mfg, sizeof(mfg));
  adv->setAdvertisementData(advData);

  // Scan response: device name only.
  NimBLEAdvertisementData scanResp;
  scanResp.setName(advName.c_str());
  adv->setScanResponseData(scanResp);

  bool fast = (flags & (ADV_FLAG_PENDING | ADV_FLAG_PAIRING)) != 0;
  adv->setMinInterval(fast ? ADV_ITVL_FAST_MIN : ADV_ITVL_SLOW_MIN);
  adv->setMaxInterval(fast ? ADV_ITVL_FAST_MAX : ADV_ITVL_SLOW_MAX);

  adv->start();
}

void bleSyncUpdatePending(uint32_t pendingCount, uint8_t batteryPct) {
  advPendingCount = pendingCount;
  advBatteryPct   = batteryPct;
  refreshAdvertising();
}

static void exitPairing() {
  if (!pairingMode) return;
  pairingMode = false;
  pendingExitPairing = false;
  // Re-baseline: any bond formed during the window is now legitimate, so it
  // must not be flagged as a "new bond outside the window" on its next auth.
  bondsBeforeAuth = NimBLEDevice::getNumBonds();
  refreshAdvertising();
  Serial.println("BLE pairing window closed");
}

void bleSyncEnterPairing() {
  pairingMode  = true;
  pairingUntil = millis() + PAIRING_WINDOW_MS;
  pendingExitPairing = false;
  bondsBeforeAuth = NimBLEDevice::getNumBonds();
  refreshAdvertising();
  Serial.println("BLE pairing window OPEN (60s, no PIN)");
}

bool bleSyncPairing() { return pairingMode; }

bool bleSyncBusy() { return connected && getState != GetState::Idle; }

void bleSyncSetDeviceId(const String &deviceId) {
  advName = deviceId;
  refreshAdvertising();
  Serial.printf("BLE device id set to %s\n", deviceId.c_str());
}

// ===========================================================================
// command dispatch
// ===========================================================================
static void dispatchCommand(const String &cmd) {
  // Split on tabs: verb [arg1 [arg2]].
  int t1 = cmd.indexOf('\t');
  String verb = t1 >= 0 ? cmd.substring(0, t1) : cmd;
  verb.trim();
  String rest = t1 >= 0 ? cmd.substring(t1 + 1) : String();
  int t2 = rest.indexOf('\t');
  String arg1 = t2 >= 0 ? rest.substring(0, t2) : rest;
  String arg2 = t2 >= 0 ? rest.substring(t2 + 1) : String();
  arg1.trim();
  arg2.trim();

  if (SD.cardType() == CARD_NONE) {
    notifyStatus("ERR\tnosd\t\n");
    return;
  }

  if (verb == "LIST") {
    handleList();
  } else if (verb == "GET") {
    uint32_t off = arg2.length() ? (uint32_t)strtoul(arg2.c_str(), nullptr, 10) : 0;
    handleGet(arg1, off);
  } else if (verb == "ACK") {
    handleAck(arg1);
  } else if (verb == "ABORT") {
    handleAbort(arg1);
  } else if (verb == "WIN") {
    long n = strtol(arg1.c_str(), nullptr, 10);
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    getWindow = (uint8_t)n;
  } else {
    notifyStatus(String("ERR\tbadcmd\t") + verb + "\n");
  }
}

void bleSyncService() {
  // --- pairing window timeout / close-on-bond ---
  if (pairingMode &&
      (pendingExitPairing || (int32_t)(millis() - pairingUntil) >= 0)) {
    exitPairing();
  }

  // --- reject a bond formed outside the pairing window (B.3) ---
  if (hasRejectPeer) {
    NimBLEAddress peer = rejectPeer;
    uint16_t h = rejectHandle;
    hasRejectPeer = false;
    Serial.println("BLE: new bond outside pairing window — deleting + disconnecting");
    NimBLEServer *srv = NimBLEDevice::getServer();
    if (srv && h != BLE_HS_CONN_HANDLE_NONE) srv->disconnect(h);
    NimBLEDevice::deleteBond(peer);
    bondsBeforeAuth = NimBLEDevice::getNumBonds();
  }

  // --- new device id from CFG write: persist + re-advertise ---
  if (hasCfgChange) {
    String id = cfgNewId;
    hasCfgChange = false;
    if (saveDeviceId(id)) {
      bleSyncSetDeviceId(id);
    }
  }

  // --- apply windowed ACK (binary) ---
  if (hasAckw) {
    uint32_t next = ackwNextSeq;
    bool nak = ackwNak;
    hasAckw = false;
    applyAckw(next, nak);
  }

  // --- pump the GET sender (window-gated; no delay() pacing) ---
  getPump();
  getDrain();

  // Device guard (B.6): if the window has stalled (no ACKW progress for 10 s)
  // there is nothing to do — getPump() naturally idles once the window fills,
  // and the phone's watchdog will ABORT/disconnect (→ getReset). No data is
  // lost; the file stays in /pending. (No active intervention required here.)

  // --- apply one pending text command ---
  if (!hasCmd) return;
  String cmd;
  if (xSemaphoreTake(cmdMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    cmd = pendingCmd;
    hasCmd = false;
    xSemaphoreGive(cmdMutex);
  } else {
    return;
  }
  dispatchCommand(cmd);
}
