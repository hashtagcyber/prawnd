# Prawnd BLE Auto-Sync Rearchitecture — Implementation Spec

Status: implementation-ready. Version 1.0. Audience: two implementation agents
(Firmware; iOS + Server). Section **B (Shared BLE Contract)** is the binding
coordination boundary — both sides implement only against it and must not need
to talk to each other. Sections C/D/E are per-agent. F/G/H apply to both.

> Hardware note (verified against `firmware/platformio.ini`): the firmware
> targets a **Seeed XIAO ESP32-C6** (not an ESP32-S3 as the brief states). The
> spec is written for the C6; everything here is SoC-agnostic except memory
> budgets, which assume the C6's single RISC-V core + ~512 KB SRAM. This does
> not change the BLE contract.

---

## A. Overview & Goals

### Target architecture

Today the device drains its on-SD `/pending/*.wav` queue via **two** uplinks: a
firmware WiFi HTTPS uploader (`uploader.cpp`, states `ApPortal`/`Uploading`) and
an iOS app over a custom BLE GATT contract. The rearchitecture **removes WiFi
entirely from the firmware** and makes **the phone-over-BLE path the sole
uplink**. The firmware no longer knows the server URL or API key — those live
only in the iOS app. The device only: records audio, queues WAVs to SD, serves
them over BLE, and advertises *whether it has pending recordings* so iOS can
discover-and-wake the app in the background.

Three roles:

- **Device (ESP32-C6, NimBLE peripheral):** records to `/pending`, advertises a
  "pending present" bit, serves a reliable chunked file-transfer GATT service,
  moves files `/pending → /uploaded` on ACK, clears the pending bit when the
  queue empties.
- **iOS app (Core Bluetooth central, background-capable):** holds the server
  credentials, auto-connects to the bonded device when it sees the pending bit
  (even when backgrounded/relaunched via Core Bluetooth **state restoration**),
  drains the queue reliably, POSTs each WAV to the server, ACKs the device.
- **Server (Node/Fastify):** unchanged contract plus **content-hash
  idempotency** so a retried background upload never double-stores.

The key constraint driving the whole design: **iOS background BLE is slow and
its wake windows are short.** Background scanning is coalesced/duty-cycled by
the OS, cannot use `CBCentralManagerScanOptionAllowDuplicatesKey`, and *requires
explicit service UUIDs in the scan filter* (a service UUID present only in the
scan-response is invisible to background scans). A background wake from a BLE
discovery grants only a few seconds of runtime — not enough to pull a multi-MB
WAV and HTTP-POST it inline. We therefore design so that:

1. The device advertises the **128-bit service UUID in the primary advertising
   packet** (not scan-response) at all times, so iOS background scan can match.
2. The **HTTP upload is handed to a background `URLSession`** (out-of-process,
   OS-managed, survives app suspension/termination), so the wake window only has
   to cover BLE pull + enqueue, not the network transfer.
3. The BLE transfer itself is **reliable and resumable** (length-prefixed,
   CRC'd, windowed-ACK chunks) so a wake window that ends mid-file can resume on
   the next wake instead of restarting.

This is the best achievable on iOS. See **H** for the honest limits (notably:
true "app fully closed" wake works only via state restoration after at least one
launch, and large files may take several wake windows).

### End-to-end happy path (ASCII sequence)

```
 USER        DEVICE (ESP32-C6)                 iOS APP (bg)              SERVER
  |   press        |                                |                       |
  |--------------->| record -> /pending/<ts>.wav    |                       |
  |   press        | patch WAV header               |                       |
  |--------------->| stopRecording()                |                       |
  |                | set PENDING bit in adv mfg-data|                       |
  |                | advertise(svcUUID in primary,  |                       |
  |                |           pending=1)            |                       |
  |                |        ~~~ adv ~~~~~~~~~~~~~~~~> | OS bg scan match      |
  |                |                                 | (relaunch app if     |
  |                |                                 |  killed, via state    |
  |                |                                 |  restoration)         |
  |                |                                 | connect(known periph) |
  |                | onConnect (encrypted, bonded)  <-| (no timeout in bg)   |
  |                | link encrypts (resolve RPA)    <-> bond resolves        |
  |                |                                 | discover svc/chars    |
  |                |                                 | subscribe DATA/STAT   |
  |                |<-- CTRL: LIST ------------------|                       |
  |                |--- STAT: F <name> <size> <crc> >|                       |
  |                |--- STAT: FEND <count> -------- >|                       |
  |                |<-- CTRL: GET <name> <off=0> --- |                       |
  |                |--- STAT: GS <name> <size> <crc> >|                      |
  |                |--- DATA: [seq|len|payload] ---->| (window of N)         |
  |                |<-- CTRL: ACKW <next_seq> ------ | (windowed ACK)        |
  |                |--- DATA: ... -----------------> | ... until EOF         |
  |                |--- STAT: GE <name> <crc_ok> -- >| verify CRC            |
  |                |                                 | enqueue bg URLSession |
  |                |                                 |---- POST /upload ----->|
  |                |                                 |   (X-Content-Sha256)  |
  |                |                                 |<--- 200 {id} / 409 ---|
  |                |<-- CTRL: ACK <name> ----------- | (after 2xx OR 409 dup)|
  |                | mv /pending/<n> -> /uploaded/<n>|                       |
  |                |--- STAT: OK <name> ----------- >|                       |
  |                | if /pending empty: PENDING bit=0|                       |
  |                | refreshAdvertising(pending=0)   |                       |
  |                |        ~~~ adv (pending=0) ~~~~> | nothing to do, idle   |
```

Fallback "manual sync": identical from `connect()` onward; just triggered by the
user tapping **Sync** in the foreground instead of by a background wake.

---

## B. Shared BLE Contract (binding coordination boundary)

Both implementers depend **only** on this section. Everything is little-endian
unless stated. "ATT payload" = negotiated MTU − 3.

### B.1 GATT service & characteristics

Reuse the existing 128-bit base UUID family `E2C5xxxx-7C6E-4F1D-9B1A-9B6A4F2D8A01`.
Existing UUIDs are kept where they still fit; new ones are added for config and
flow control. **Uppercase is canonical; firmware string literals may be
lowercase — they are the same UUID.**

| Name   | UUID                                   | Properties                          | Encryption | Direction / purpose |
|--------|----------------------------------------|-------------------------------------|------------|---------------------|
| SVC    | `E2C50000-7C6E-4F1D-9B1A-9B6A4F2D8A01` | (service)                           | —          | Prawnd Sync service |
| CTRL   | `E2C50001-7C6E-4F1D-9B1A-9B6A4F2D8A01` | Write, WriteNR, **WriteEnc**        | required   | phone → device commands (incl. windowed ACK) |
| STAT   | `E2C50002-7C6E-4F1D-9B1A-9B6A4F2D8A01` | **Notify**, ReadEnc                 | required\* | device → phone control/status (text lines) |
| DATA   | `E2C50003-7C6E-4F1D-9B1A-9B6A4F2D8A01` | **Notify**                          | required\* | device → phone framed file chunks |
| CFG    | `E2C50004-7C6E-4F1D-9B1A-9B6A4F2D8A01` | **ReadEnc, WriteEnc**               | required   | device config (device_id, fw version, caps) |

\* STAT/DATA notifications inherit link encryption because the link is forced
encrypted before any command is accepted (CTRL is WriteEnc). The CCCDs are
written by the central after the link is encrypted.

**MTU / chunk negotiation.** Device requests ATT MTU 517 at init; iOS negotiates
down (typically 185 foreground, often 23–185 background). Both sides compute the
DATA payload size as `mtu - 3 - DATA_HEADER (4)` (see B.4). The device exposes
the *current* negotiated MTU back to the app via the `GS` line's `<mtu>` field so
the app can sanity-check. Neither side hardcodes a chunk size.

### B.2 Advertising format (load-bearing for background wake)

Primary advertising packet (≤31 bytes), built every time pending-state or
pairing-state changes:

```
Flags (LE General Disc + BR/EDR unsupported)            3 bytes
Complete 128-bit Service UUID = SVC                    18 bytes   <-- MUST be in PRIMARY pkt
Manufacturer Specific Data                              7 bytes
  -> company id 0xFFFF (2) + payload (4):
       byte[0] = PROTO_VER   (0x02 for this spec)
       byte[1] = FLAGS bitfield:
                   bit0 (0x01) = PAIRING window open
                   bit1 (0x02) = PENDING recordings present
                   bit2 (0x04) = BUSY (a sync is in progress)
       byte[2] = pending count, clamped 0..255
       byte[3] = battery percent 0..100 (0xFF = unknown)
Total: 3 + 18 + 9 = 30 bytes  (fits 31)
```

Scan response: device **name** only (`Complete Local Name`).

Rules:
- The SVC UUID **must** be in the primary packet (not scan-response) so iOS
  background scanning — which only honours the primary packet and requires an
  explicit service-UUID filter — can match. (The current firmware already does
  this; keep it.)
- `PROTO_VER` byte lets the app detect old firmware and fall back to the legacy
  text protocol if desired (recommended: app refuses < 0x02 and shows "update
  firmware"). Old firmware used a 3-byte mfg payload `{0xFF,0xFF,pairingflag}`;
  the new layout is `{0xFFFF company}{ver,flags,count,batt}`. They are
  distinguishable by length (3 vs 4 payload bytes) and by company-id placement.
- Advertise **continuously** in every non-recording state. Pending bit toggles
  the FLAGS; the app distinguishes "wake me, there's work" (bit1=1) from "idle".
- Advertising interval: idle (pending=0) **1000–1285 ms** (power saving, still
  discoverable); pending=1 or pairing **152.5–211.25 ms** (fast discovery). The
  device bumps to the fast interval whenever bit1 or bit0 is set.

### B.3 Bonding / security model

- LE Secure Connections, **Just Works** (no passkey), bonding enabled.
  `setSecurityAuth(bond=true, mitm=false, sc=true)`, IO cap `NO_INPUT_NO_OUTPUT`.
- **New bonds are accepted only while the PAIRING window is open** (opened by a
  triple button press, 60 s). Outside the window, the device still advertises and
  still accepts connections from **already-bonded** centrals.
- **Bug #1 fix — RPA / reconnect:** Do **not** gate `onConnect` on
  `info.isBonded()` (unreliable: iOS uses rotating Resolvable Private Addresses
  and a not-yet-resolved RPA reads as unbonded at connect time). Instead:
  1. Configure the NimBLE **resolving list** and persist the **IRK** at bond
     time so the controller can resolve the peer's RPA on reconnect
     (`NimBLEDevice::setSecurityAuth` with bonding + ensure bonds persist in NVS;
     do **not** call anything that wipes bonds at boot).
  2. Enforce access via the **GATT layer, not the connect callback**: CTRL is
     `WriteEnc`, so an unbonded/unencrypted central simply cannot issue any
     command (the stack rejects the write with Insufficient Authentication and
     iOS auto-initiates pairing). In `onConnect`, **only** reject when
     `pairingMode == false` **and** the new pairing is not already encrypted —
     and even then, prefer to let encryption be demanded lazily by the WriteEnc.
     Concretely: remove the `!info.isBonded()` disconnect; rely on WriteEnc +
     the pairing-window gate on *new bond creation* (reject `onAuthentication`
     /new bonding outside the window, not reconnects of existing bonds).
- Encryption is required before LIST/GET/ACK/CFG are honoured (enforced by
  WriteEnc on CTRL and ReadEnc/WriteEnc on CFG/STAT).

### B.4 Wire protocol

Two planes: **STAT** carries newline-terminated, tab-delimited **text** control
lines (human-debuggable, evolves the existing protocol). **DATA** carries
**binary framed** file chunks. **CTRL** carries phone→device commands as text,
plus the binary windowed-ACK.

#### B.4.1 CTRL commands (phone → device)

Text, UTF-8, tab-delimited, **no trailing newline required**. One command per
write.

| Command                         | Meaning |
|---------------------------------|---------|
| `LIST`                          | enumerate `/pending`. Device replies on STAT with `F …` lines then `FEND`. |
| `GET\t<name>\t<offset>`         | begin/resume sending `<name>` starting at byte `<offset>` (0 = from start). Device replies `GS …` then streams DATA frames from `<offset>`. |
| `ACK\t<name>`                   | mark `<name>` uploaded → move `/pending → /uploaded`. Device replies `OK`/`ERR`. |
| `ABORT\t<name>`                 | cancel an in-progress GET (e.g. wake window ending). Device stops streaming, replies `GA\t<name>`. |
| `WIN\t<n>`                      | (optional) set window size to `n` frames (1..32). Default 8. Persists for the connection. |

Plus the **binary** flow-control write on CTRL (distinguished from text by first
byte `0x01`, which is not a printable command char):

```
ACKW frame (phone -> device, on CTRL, WriteNR):
  byte[0]      = 0x01           (ACKW opcode marker)
  byte[1..4]   = next_seq (uint32 LE)   -- lowest seq number NOT yet received
                                           contiguously (cumulative ACK)
  byte[5]      = flags: bit0 = NAK (request retransmit from next_seq)
```

#### B.4.2 STAT lines (device → phone)

ASCII, `\t`-delimited, `\n`-terminated, one logical record per notification
(may be coalesced by the app's line buffer — the app MUST buffer and split on
`\n`, as it does today).

| Line                                   | Meaning |
|----------------------------------------|---------|
| `F\t<name>\t<size>\t<crc32>\n`         | one pending file. `<crc32>` = CRC-32 (see B.4.4) of the whole file, hex, lowercase, 8 chars. Enables the app to dedup/verify without a full GET. |
| `FEND\t<count>\n`                      | end of LIST. |
| `GS\t<name>\t<size>\t<crc32>\t<mtu>\t<chunk>\n` | start of GET. `<chunk>` = DATA payload bytes/frame the device will use. |
| `GE\t<name>\t<crc_ok>\n`               | end of file reached. `<crc_ok>` = `1` (device-side running CRC matched header CRC) or `0`. |
| `GA\t<name>\n`                         | GET aborted (response to `ABORT`). |
| `OK\t<name>\n`                         | ACK succeeded (file moved to `/uploaded`). |
| `ERR\t<code>\t<name>\n`                | error; see B.4.5. `<name>` may be empty. |

#### B.4.3 DATA frames (device → phone, binary)

Each DATA notification is exactly one frame:

```
DATA frame layout:
  byte[0..3]  = seq      (uint32 LE)   -- 0-based frame index within this GET
  byte[4..5]  = len      (uint16 LE)   -- payload byte count (1..chunk)
  byte[6..N]  = payload  (len bytes of raw WAV file data)
Header = 6 bytes. Payload max = (negotiated MTU - 3) - 6.
```

- Frames are sent in strictly increasing `seq`. Byte offset of frame `seq` =
  `GET.offset + Σ(prior payload lens)`; because every frame except the last is
  exactly `<chunk>` bytes, `byte_offset = offset + seq*chunk`. The last frame may
  be shorter. (Keeping all-but-last full-size makes offset math O(1) for resume.)
- No CRC per frame (CRC is per-file, verified at `GE`). `len` + `seq` detect
  truncation/reorder/loss.

#### B.4.4 CRC

CRC-32 (IEEE 802.3, polynomial `0xEDB88320`, init `0xFFFFFFFF`, reflected,
final XOR `0xFFFFFFFF`) over the **entire file bytes** (WAV header included).
Emitted lowercase hex, zero-padded to 8 chars. Both sides compute identically.
The device computes file CRCs lazily and **caches** them (filename+size+mtime →
crc) so `LIST` is cheap on repeat (see C).

#### B.4.5 Error codes (`ERR\t<code>`)

| code        | meaning |
|-------------|---------|
| `nosd`      | no SD card present |
| `notfound`  | file not in `/pending` |
| `badname`   | name failed validation (`.`-prefixed, non-`.wav`, path sep) |
| `badoffset` | requested offset > file size |
| `rename`    | SD rename failed during ACK |
| `busy`      | another GET in progress (only one concurrent GET) |
| `internal`  | unexpected SD/read error |

### B.5 Reliable windowed transfer — exact state machines

One GET active per connection. Window size `W` (default 8, set via `WIN`).

**Device (sender) state machine for a GET:**

```
States: IDLE -> STREAMING -> DRAINING -> DONE/ABORTED

on CTRL "GET name offset":
   validate name/offset; if bad -> STAT ERR; stay IDLE
   open file, seek(offset)
   compute/lookup file size + crc32
   STAT: GS name size crc mtu chunk
   base = offset/chunk ; next_seq = base ; win_lo = base
   running_crc = (offset==0 ? fresh : recompute up to offset)   // see note
   state = STREAMING

STREAMING loop (cooperative, in bleSyncService, NOT a BLE callback):
   while next_seq < win_lo + W  and  not EOF  and  connected:
       read chunk bytes
       send DATA frame(seq=next_seq, payload)
       next_seq++
       if notify queue full -> break (retry next service tick)
   if EOF and next_seq == total_frames:
       state = DRAINING

on CTRL ACKW(next_ack, flags):
   if flags.NAK: next_seq = next_ack; reseek file to next_ack*chunk   // retransmit
   win_lo = next_ack                 // slide window; allows more sends
   (loop above resumes on next service tick)

DRAINING:
   when win_lo >= total_frames (all frames cumulatively ACKed):
       STAT: GE name crc_ok
       close file; state = IDLE

on CTRL ABORT name:  stop, close file, STAT GA name, state = IDLE
on disconnect:       close file, state = IDLE  (no data lost; /pending intact)
```

> Note on resume CRC: the device's `crc_ok` at `GE` is computed over the bytes it
> *actually sent this session starting at offset*. For a resumed GET
> (offset>0) the device cannot cheaply recompute the prefix CRC, so it sends the
> **full-file cached CRC** in `GS`/`GE` and sets `crc_ok=1` if the file is intact
> on disk (it always is — it's a closed file). **Authoritative verification is
> the app's**: the app concatenates the resumed bytes onto whatever it already
> had and checks the full-file CRC from `GS`. `crc_ok` is advisory.

**App (receiver) state machine for a GET:**

```
States: REQUESTED -> RECEIVING -> VERIFYING -> COMPLETE / FAILED

send CTRL "GET name offset"   (offset = bytes already persisted for name, else 0)
start per-GET watchdog timer (see B.6)

on STAT GS name size crc mtu chunk:
   expected_size=size; file_crc=crc; chunk=chunk
   expect_seq = offset/chunk
   buffer = (resume? existing partial : empty)

on DATA frame(seq,len,payload):
   reset watchdog
   if seq == expect_seq:
        append payload; expect_seq++
        every W/2 frames OR every 200ms: send ACKW(next=expect_seq)
   elif seq < expect_seq:  ignore (dup)
   else (seq > expect_seq): gap detected
        send ACKW(next=expect_seq, NAK)   // ask device to rewind
   if bytes_received >= expected_size: state = VERIFYING

on STAT GE name crc_ok:
   state = VERIFYING

VERIFYING:
   compute CRC-32 over assembled file bytes
   if == file_crc: COMPLETE -> hand to uploader, then send CTRL ACK name on 2xx
   else: persist partial (for resume), FAILED -> retry GET from last good offset

on STAT ERR / disconnect / watchdog:
   persist partial bytes to disk keyed by name (resume point), FAILED
```

### B.6 Timeouts (bug #2 fix) — both ends

The app sets a watchdog on **every** outstanding operation, not just connect:

| Operation        | App-side timeout | On expiry |
|------------------|------------------|-----------|
| connect          | 12 s (fg), **none** (bg restoration — let OS hold it) | fg: fail; bg: leave pending connect |
| service/char discovery + subscribe | 8 s | fail GET/sync |
| LIST (CTRL→FEND) | 8 s | fail, disconnect |
| GET (per-frame inactivity) | 5 s since last DATA/STAT | NAK once, then fail+persist partial |
| GET (overall)    | 60 s | abort GET, persist partial, move on |
| ACK (CTRL→OK)    | 8 s | treat as failed; do **not** re-upload (idempotency covers a possibly-applied ACK) |

The device has its own guards: if `STREAMING`/`DRAINING` and no ACKW progress
for 10 s, it stops streaming (window stalled) and waits; on disconnect it resets
to IDLE. The device never blocks the main loop > a few ms per service tick.

### B.7 Config flow over BLE (CFG characteristic)

CFG is a single Read/Write-encrypted characteristic carrying a small UTF-8 JSON
object (≤ 256 bytes; fits one or two ATT reads — app uses `readValue`, which
CoreBluetooth reassembles).

Read (device → app), JSON:
```json
{ "device_id":"prawnd-AB12", "fw":"2.0.0", "proto":2, "free_mb":1234, "pending":3 }
```

Write (app → device), JSON; only present keys are applied & persisted to NVS:
```json
{ "device_id":"my-recorder-1" }
```
- `device_id`: validated `^[A-Za-z0-9_-]{1,32}$`. On change, device persists to
  NVS (`config.cpp`), updates the advertised name, and the new id takes effect
  immediately (no reboot needed for the name; BLE re-advertises).
- Unknown keys ignored. Server URL / API key are **never** sent to the device.

This replaces the deleted captive portal as the *only* configuration surface.

---

## C. Firmware Spec (prawnd repo)

Target file: `firmware/`. Board: XIAO ESP32-C6 (`platformio.ini`).

### C.1 Files to DELETE

```
firmware/src/wifi_mgr.cpp
firmware/src/wifi_mgr.h
firmware/src/portal.cpp
firmware/src/portal.h
firmware/src/uploader.cpp
firmware/src/uploader.h
```

Symbols removed (no other consumers — verified, all only used by `main.cpp`):
`wifiTryStation`, `wifiStartAp`, `wifiInStaMode`, `wifiCurrentIp`,
`portalBegin`, `portalEnd`, `portalSaveRequested`, `uploadFile`.

### C.2 `platformio.ini` changes

- Remove `esp32async/ESPAsyncWebServer` and `esp32async/AsyncTCP` from
  `lib_deps` (only the portal used them). Keep `ArduinoJson` (now used by CFG)
  and `NimBLE-Arduino`.
- Remove `<WiFi.h>` usage. Note: on ESP32, NimBLE and WiFi share nothing
  required here; dropping WiFi reduces RAM/flash. `huge_app.csv` can stay or be
  reduced — keep `huge_app.csv` to avoid a repartition/erase churn (document it,
  but no need to change).
- `config.cpp` includes `<WiFi.h>` **only** for `WiFi.macAddress()` which it
  already avoids (it uses `ESP.getEfuseMac()`); drop the `#include <WiFi.h>`.

### C.3 `config.*` changes

New `Config` (drop WiFi fields):
```cpp
struct Config {
  String device_id;                 // NVS key "dev"
  bool valid() const { return device_id.length() > 0; }
};
```
- `loadConfig`: read only `"dev"`; default via `defaultDeviceId()` (keep eFuse
  MAC scheme `prawnd-XXXX`).
- `saveConfig`: persist only `"dev"`.
- Add `bool saveDeviceId(const String&)` convenience used by the CFG write path.
- `clearConfig()` unchanged (still wipes the `prawnd` NVS namespace on long
  press) — but note NimBLE bonds live in a **separate** NVS namespace managed by
  the BLE stack; long-press factory reset SHOULD also clear bonds
  (`NimBLEDevice::deleteAllBonds()`), document this.
- Drop keys `ssid`,`psk`,`url`,`key` (old NVS values become dead — harmless;
  `clearConfig` removes them; no migration needed).

### C.4 New main-loop state machine (`main.cpp`)

```cpp
enum class State { Boot, Idle, Recording };   // ApPortal/StaIdle/Uploading removed
```

Removed: `#include "wifi_mgr.h" / "portal.h" / "uploader.h"`,
`tryUploadOnce()`, `pendingUploadPath`, `lastDrainAt`, `firstDrain`,
`portalSaveRequested()` handling, all WiFi/AP boot logic.

`setup()`:
```
Serial, button, SPI, SD.begin(1MHz), ensureDirs()
#ifdef ENABLE_BATTERY: batteryBegin()/read()
loadConfig(cfg)                  // device_id only
bleSyncBegin(cfg.device_id)      // sets up GATT incl. CFG, resolving list, adv
updatePendingAdvertising()       // set PENDING bit per /pending contents at boot
state = State::Idle
```

`loop()`:
```
if buttonLongPressed():   clearConfig(); NimBLEDevice::deleteAllBonds(); restart
if buttonTriplePressed() && state != Recording:  bleSyncEnterPairing()

switch(state):
  Boot:      delay(20)
  Idle:
     bleSyncService();                 // drains one BLE step (LIST/GET window/ACK/CFG)
     if buttonShortPressed(): startRecording()
     else delay(5)                     // tighter loop so streaming stays fed
  Recording:
     // audio+SD in FreeRTOS tasks; BLE intentionally NOT serviced here
     if buttonShortPressed(): stopRecording()
     else delay(20)
```

`startRecording()`: unchanged pipeline, plus: before starting, the device should
**stop advertising the BUSY/streaming** and may keep advertising the service
(recording does not need BLE). Keep it simple: during Recording, do not service
BLE commands (matches today's behaviour); a connected phone's in-flight GET will
time out and resume later. Set FLAGS BUSY bit cleared.

`stopRecording()`: unchanged through `SD.rename` to `/pending/<ts>.wav`, then:
```
state = State::Idle
updatePendingAdvertising()    // sets PENDING bit=1, count, fast adv interval
```

### C.5 `ble_sync.*` rewrite

Add CFG characteristic; add binary DATA framing + windowed ACK; fix security.

New public API (`ble_sync.h`):
```cpp
void bleSyncBegin(const String &deviceId);
void bleSyncService();              // cooperative: advances LIST/GET/ACK/CFG + streaming window
bool bleSyncBusy();                 // connected AND a GET/transfer is active
void bleSyncEnterPairing();
bool bleSyncPairing();
void bleSyncSetDeviceId(const String&);   // applied from CFG write
// advertising helper used by main.cpp:
void bleSyncUpdatePending(uint32_t pendingCount, uint8_t batteryPct);
```

Implementation notes:

- **Advertising** (`refreshAdvertising`) builds the B.2 layout: SVC UUID in the
  primary packet, mfg-data `{0xFFFF, ver=2, flags, count, batt}`. Helper
  `bleSyncUpdatePending(count,batt)` recomputes flags (PENDING bit from
  `count>0`), sets the adv interval fast/slow accordingly, and restarts adv.
  Called from the main loop only (never from a BLE callback — keep today's rule).
- **Pending count** is computed by scanning `/pending` for valid `.wav` files
  (reuse `isRecordingName`). Cache the count; recompute after ACK and after
  `stopRecording`.
- **Security setup:** `setSecurityAuth(true,false,true)`,
  `setSecurityIOCap(NO_INPUT_NO_OUTPUT)`. **Remove the
  `!info.isBonded()` disconnect in `onConnect`** (bug #1). Gate *new bond
  creation* to the pairing window: in `onAuthenticationComplete`, if a **new**
  bond formed while `pairingMode==false`, delete that bond and disconnect; if it
  formed during the window, keep it and close the window. Ensure
  `NimBLEDevice::init` does not erase bonds; verify bonds persist across reboot
  (NimBLE stores them in NVS by default). Configure the resolving list so RPAs
  resolve — with NimBLE this is automatic once a bond with an IRK exists and
  `setSecurityAuth` enables it; do **not** add any code that clears
  `ble_store`. CTRL stays `WRITE_ENC` so unencrypted centrals cannot command.
- **Command handling** stays single-threaded: BLE callbacks only enqueue; all SD
  access happens in `bleSyncService()` from the main loop (preserve the mutex +
  `pendingCmd` pattern). Extend to parse the new commands (`GET name offset`,
  `ABORT`, `WIN`) and the binary `ACKW` on CTRL (detect first byte `0x01`).
- **LIST** emits `F name size crc` — compute CRC via the cache (C.6).
- **GET sender:** implement the B.5 device state machine. Replace the old blind
  `delay(6)` notify pacing with **window-gated** sending: send up to `win_lo+W`
  unacked frames, then wait for ACKW to slide the window. Use
  `dataChar->notify()` return / NimBLE's notify queue back-pressure: if a notify
  fails (buffers full), break and retry on the next service tick. Frame header
  is the 6-byte `{seq,len}` per B.4.3. **Remove `delay(6)` entirely.**
- **ACK** handler unchanged logic (rename `/pending → /uploaded`), then call
  `bleSyncUpdatePending(newCount,batt)` so the advertised PENDING bit clears when
  the queue empties.
- **CFG:** `ReadEnc` callback returns the JSON in B.7 (build with ArduinoJson);
  `WriteEnc` callback parses JSON, validates+persists `device_id`, calls
  `bleSyncSetDeviceId`. Keep all CFG work to enqueue-then-service if it touches
  SD/NVS heavily (NVS write is fine in callback but prefer servicing).

### C.6 CRC cache (SD/memory constraint)

Computing CRC-32 over a multi-MB WAV on every LIST is too slow on a 1 MHz SPI
SD. Maintain an in-RAM cache: `struct { String name; uint32_t size; uint32_t crc; }`
for up to ~16 entries (the pending queue is small). On `stopRecording`, compute
the new file's CRC once (streamed, 4 KB reads, ~tens of ms/MB acceptable at stop
time) and insert. On LIST, use cache; on miss (e.g. files copied onto the card),
compute lazily and cache. Persisting the cache is optional (recompute on boot is
acceptable since boots are rare). Memory: 16 × ~40 B ≈ negligible.

### C.7 Memory / SD / concurrency / power

- **Concurrency:** one GET at a time (`ERR busy` otherwise). All SD I/O on the
  main loop via `bleSyncService`; recording uses the existing two FreeRTOS tasks
  and BLE is **not** serviced during Recording — no SD contention.
- **Buffers:** DATA frame staging buffer = one chunk (≤ MTU−3−6 ≤ ~508 B);
  reuse a single static buffer. SD read buffer 4 KB for CRC. StreamBuffer (64 KB)
  only exists during Recording.
- **Notify back-pressure:** rely on NimBLE's `notify()` returning failure when
  the controller TX queue is full; never `delay()` to pace. The window scheme
  (W=8) bounds in-flight frames so the queue can't be flooded.
- **Power:** idle advertising at 1 s interval is the dominant idle draw; the fast
  interval only runs while pending>0 (i.e., until the phone drains). After the
  queue empties the device returns to slow advertising. Recording disables BLE
  servicing. (No sleep modes specified here; out of scope, see H.)

---

## D. iOS App Spec (plagueboxmobile repo)

Target: `native/ios/PrawndSync/PrawndSync/`. Deployment target iOS 17,
bundle id `io.cobl.dev.prawndsync`.

### D.1 Capabilities, Info.plist, entitlements

`Info.plist` additions:
```xml
<key>UIBackgroundModes</key>
<array>
  <string>bluetooth-central</string>
</array>
<key>NSBluetoothAlwaysUsageDescription</key>
<string>Prawnd Sync connects to your recorder over Bluetooth — including in the
background — to pull and upload your recordings automatically.</string>
```
- Keep existing `NSAppTransportSecurity / NSAllowsLocalNetworking` (LAN servers).
- No special entitlement file is required for `bluetooth-central` background mode
  (it is an Info.plist background mode, not an entitlement). Background
  `URLSession` also needs no entitlement.
- Enable the **Background Modes** capability in the target (adds the above key
  via Xcode); the project currently has no `.entitlements` file — none needs to
  be added for this feature.

### D.2 Core Bluetooth state restoration

Create the `CBCentralManager` with a **restore identifier** and implement
`willRestoreState`. This is what lets iOS **relaunch the app into the background**
when the bonded device is discovered after the app was terminated.

```swift
central = CBCentralManager(
    delegate: self,
    queue: .main,
    options: [CBCentralManagerOptionRestoreIdentifierKey: "io.cobl.dev.prawndsync.central"])
```

Implement:
```swift
func centralManager(_ c: CBCentralManager, willRestoreState dict: [String: Any]) {
   // Re-adopt restored peripherals; re-wire delegate; resume in-flight sync.
   if let ps = dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral] {
       for p in ps where p.identifier.uuidString == SettingsManager.shared.pairedDeviceID {
           peripheral = p; p.delegate = self
           // if state == .connecting/.connected continue handshake
       }
   }
}
```

On `centralManagerDidUpdateState(.poweredOn)`:
- If a device is paired, **register a background discovery**:
  `central.scanForPeripherals(withServices: [svcUUID], options: nil)` — note
  **no `AllowDuplicates`** (ignored in background) and the service-UUID filter is
  **mandatory** for background scans.
- Also issue a standing `central.connect(knownPeripheral, options:
  [CBConnectPeripheralOptionNotifyOnConnection/Disconnection: true])` with **no
  timeout** for the paired peripheral (retrieved via
  `retrievePeripherals(withIdentifiers:)`). A pending connect persists across
  suspension; iOS completes it (and wakes the app) when the device is in range —
  this is the most reliable background wake, more so than scanning.

Recommended primary mechanism: **`connect` with no timeout to the known
peripheral** (works even when the device is just advertising and the app is
suspended), **backed by** the service-UUID background scan for the relaunch /
discovery case. Use both.

### D.3 Background execution strategy (the hard part — design around limits)

Hard limits (state them plainly):
- A BLE-triggered background wake gives only a **few seconds** of foreground-
  equivalent runtime. Not enough for a multi-MB BLE pull *and* an HTTP upload.
- `BGProcessingTask` can run longer but is **opportunistic** (the OS schedules it
  when it wants, typically on charge/at night) — unsuitable as the *only* path
  for "upload right after I recorded."

Chosen approach (concrete):
1. **BLE pull in the wake window.** On wake (connect/discovery), run the BLE
   drain. Pull as many files as the window allows. The **reliable resumable
   transfer (B.5)** means a file interrupted by the window ending is **persisted
   partially and resumed** on the next wake — no restart cost.
2. **HTTP upload via a background `URLSession`.** Do **not** upload inline with
   `URLSession.shared.data(for:)`. Instead use a single shared
   `URLSession(configuration: .background(withIdentifier:
   "io.cobl.dev.prawndsync.upload"))` with `isDiscretionary = false`,
   `sessionSendsLaunchEvents = true`. Write each pulled WAV to a temp file and
   create an **upload task from file** (`uploadTask(with:fromFile:)`). The OS
   performs the transfer **out of process**, surviving app suspension/termination,
   and relaunches the app via
   `application(_:handleEventsForBackgroundURLSession:completionHandler:)` /
   the SwiftUI `.backgroundTask(.urlSession(...))` modifier when done.
3. **ACK after upload confirmation, not after pull.** The device file stays in
   `/pending` until the server returns 2xx (or 409 duplicate). On the
   `URLSessionTaskDelegate didCompleteWithError` callback (which may fire in a
   *later* relaunch), the app reconnects BLE if needed and sends `ACK <name>`.
   To survive app death between upload-complete and ACK, persist a small
   **"awaiting ACK" set** (file name + device id) in UserDefaults; on launch /
   wake, drain it.
4. **`BGProcessingTask` as a safety net.** Register
   `io.cobl.dev.prawndsync.refresh` to periodically reconnect and drain anything
   stuck (device out of range during the original wake, etc.). Best-effort only.

Tradeoffs / honest limits → **H**.

### D.4 Redesigned `BLESyncManager`

Keep the `@MainActor`, continuation-based structure; change:

- **Auto-trigger on wake.** New entry point `func autoSyncIfPending(_ p:
  CBPeripheral, advFlags: UInt8?)`. Called from `didDiscover` (background scan
  match) and from `didConnect` (standing connect completed). If `advFlags`
  shows PENDING (bit1) or unknown, connect (if needed) and run the drain. The
  manual `sync()` remains and calls the same `runSync()`.
- **Per-command timeouts (bug #2 fix).** Wrap **every** continuation
  (`listCont`, `getCont`, `ackCont`, discovery/subscribe) in a watchdog per B.6,
  not just connect. Implement a helper:
  ```swift
  func withTimeout<T>(_ secs: Double, _ op: () async throws -> T) async throws -> T
  ```
  In **background**, the connect itself uses **no** app-side timeout (let iOS
  own it); per-command timeouts still apply once connected.
- **`subscribedCount` fix (bug #2).** Only increment on
  `didUpdateNotificationStateFor` when `error == nil`; require **both** STAT and
  DATA subscribed (count==2) **and** `ctrlChar != nil` before `resumeConnect()`.
  On subscription error, fail the connect continuation.
- **`deviceName` fix (bug #2).** On the `retrievePeripherals` fast path (no
  advertisement seen), set `deviceName = SettingsManager.shared.pairedDeviceName`
  immediately so uploads are filed under the real device id, never the literal
  `"prawnd"`. Better: stop using `deviceName` for `X-Device-Id` at all — read the
  authoritative id from the **CFG characteristic** once after connect and cache
  it (`SettingsManager.pairedDeviceID`-adjacent: store `pairedDeviceCfgId`). Use
  that for `X-Device-Id`. Falls back to `pairedDeviceName` then `"prawnd"`.
- **Reliable receiver (bug #3 fix).** Replace `handleData`/`finishGet` with the
  B.5 app receiver: parse the 6-byte DATA frame header, track `expect_seq`,
  send `ACKW` every `W/2` frames or 200 ms, NAK on gap, persist partial to disk
  for resume, verify full-file CRC at `GE`, retry/resume on mismatch or
  timeout. Drop the old "count bytes until >= size" heuristic.
- **Resume support.** Persist partial downloads to
  `FileManager … /Caches/partial/<deviceId>/<name>.part` plus a sidecar with
  bytes-received. On GET, send `offset = existing partial size`. On COMPLETE,
  move `.part` → temp and hand to the background uploader.
- **CFG read.** After connect+subscribe, `readValue(for: cfgChar)`; parse JSON;
  cache `device_id`, `fw`, `proto`. If `proto < 2`, surface "update firmware"
  and stop (do not attempt the new framing against old firmware).
- **One-GET discipline.** Serialize GETs (the device only does one).

### D.5 Uploader changes (`PrawndUploader.swift`)

- Add a background-`URLSession`-based path: `enqueueUpload(name:fileURL:deviceId:
  sha256:)` that builds the same request (Bearer, `audio/wav`, `X-Device-Id`,
  `X-Client-Timestamp`) **plus** a new header **`X-Content-Sha256: <hex>`** (see
  E) and creates `uploadTask(with:fromFile:)` on the shared background session.
- Keep the existing synchronous `upload(...)` for the foreground manual path /
  unit tests, but route auto-sync through the background session.
- Implement `URLSessionDelegate`/`URLSessionTaskDelegate` (likely on a small
  dedicated object owned by the app, since the session must outlive views):
  `didCompleteWithError` → on success/409 mark "awaiting ACK"; on transient
  failure the background session retries automatically.
- Compute SHA-256 of the WAV (CryptoKit `SHA256`) for the idempotency header and
  to dedup locally.

### D.6 UI changes

- `SyncView`: add an **auto-sync status** line ("Auto-sync on — last synced N min
  ago / N pending"). Keep the **Sync** button as a manual fallback (calls
  `ble.sync()`). Remove copy that implies sync only happens when the user taps.
- `PairView`: filter on the new mfg-data — **PAIRING bit = byte[1] bit0** (the
  layout moved; old code read byte index 2 of a 3-byte payload; new payload is
  `{ver,flags,count,batt}` after the 2-byte company id, so within
  `CBAdvertisementDataManufacturerDataKey` the bytes are `[0xFF,0xFF,ver,flags,
  count,batt]` → **flags is index 3, PAIRING = `mfg[3] & 0x01`**). Update the
  `pairingFlag` extraction accordingly.
- `SettingsView`: add an optional **device_id** editor that writes the CFG
  characteristic (connect, write JSON, disconnect). Server URL / API key
  unchanged. Show firmware version (from CFG).
- `SettingsManager`/`APIKeyManager`: **unchanged** (Keychain service
  `io.cobl.dev.prawndsync`/account `api_key`, accessible AfterFirstUnlock — which
  is correct for background use; do not change to WhenUnlocked or background
  uploads break while the phone is locked). Add a `pairedDeviceCfgId` and a
  small persisted "awaiting ACK" list (UserDefaults).

### D.7 Edge cases

| Case | Behaviour |
|------|-----------|
| App killed by user | iOS won't relaunch for BLE until the app is launched once more; after that, state restoration + standing connect resume background wake. Document for users (manual launch once after force-quit). |
| Bluetooth off | `centralManagerDidUpdateState` → `.poweredOff`; show status; auto-resume when back on. |
| Out of range during wake | Standing no-timeout `connect` stays pending; completes later. `BGProcessingTask` net catches stragglers. |
| Partial transfer | Persisted `.part` + offset; resumed via `GET name <offset>`; CRC verified at end. |
| Duplicate upload | Background session may retry; server dedups on `X-Content-Sha256` (E). App still ACKs the device on 200 **or** 409. |
| Upload completes but app dies before ACK | "Awaiting ACK" set persisted; drained on next launch/wake → reconnect, send `ACK`. Device idempotent (file already moved → `ERR notfound` is treated as success). |
| Device records again mid-drain | New file appears in next LIST; advertised count increments; handled next pass. |
| Two GETs requested | App serializes; device returns `ERR busy` if violated. |

---

## E. Server Spec (prawnd/server)

Current `/upload` (verified `server/src/routes/upload.js`): POST, auth via
`Authorization: Bearer` / `X-Api-Key` / `?api_key` matching `PRAWND_API_KEY`;
body is raw `audio/wav`/`application/octet-stream` (200 MB limit); headers
`X-Device-Id` (sanitized, default `unknown`) and `X-Client-Timestamp` (→
`client_ts`); generates a fresh **ULID per request**, stores
`audio/<device>/<yyyy>/<mm>/<id>.wav`, computes and stores **`sha256`** in the
`recordings` row, runs inline denoise → `<id>.cleaned.wav`. **It is NOT
idempotent today** — re-posting the same WAV creates a second row/file. SHA-256
is computed but never used for dedup.

Minimal change — **content-hash idempotency**:

1. Accept an optional request header **`X-Content-Sha256: <hex64>`**. If absent,
   server computes it from the body as today (still authoritative — server
   re-hashes regardless; the header is an optimization/assertion).
2. After hashing, **before inserting**, look up an existing recording with the
   same `(device_id, sha256)`:
   - Add a unique index:
     `CREATE UNIQUE INDEX IF NOT EXISTS uniq_recordings_device_sha
        ON recordings(device_id, sha256);`
     (in `db.js` migration; existing dup rows, if any, must be de-duplicated
     once before adding — document a one-time cleanup or use
     `CREATE UNIQUE INDEX … ` guarded by a pre-check).
   - If a row exists: **do not** store a second file; return **HTTP 409** (or
     **200** with the existing record + `"duplicate": true`). **Recommendation:
     return 200 with `{ "id": <existingId>, "duplicate": true, ... }`** so the
     client treats it as success and ACKs the device. (Simpler client logic than
     409.) If you prefer 409, the client (D) already treats 409 as ACK-worthy.
3. If `X-Content-Sha256` is present and mismatches the server-computed hash,
   return **400 `sha_mismatch`** (detects corruption in transit; the BLE CRC
   already guards the device→phone hop, this guards phone→server).
4. Keep everything else (auth, denoise, WAV parse, response shape) unchanged.
   Add `duplicate` (bool) to the success JSON.

No change to auth, body parsing, or storage layout. This makes background
retries and the "uploaded-but-ACK-lost" edge safe.

---

## F. Migration / Removal Checklist

**Firmware — delete files:**
- `firmware/src/wifi_mgr.cpp`, `wifi_mgr.h`
- `firmware/src/portal.cpp`, `portal.h`
- `firmware/src/uploader.cpp`, `uploader.h`

**Firmware — remove symbols / code:**
- `main.cpp`: includes of the three deleted headers; `State::ApPortal`,
  `State::StaIdle`, `State::Uploading`; `tryUploadOnce()`; `pendingUploadPath`,
  `lastDrainAt`, `firstDrain`; `portalSaveRequested()` block; WiFi/AP boot block.
- `config.h/.cpp`: drop `ssid`,`psk`,`upload_url`,`api_key` fields and NVS keys
  `"ssid"`,`"psk"`,`"url"`,`"key"`; `valid()` now checks `device_id` only.
- `platformio.ini`: drop `ESPAsyncWebServer`, `AsyncTCP` from `lib_deps`; drop
  `#include <WiFi.h>` from `config.cpp`.

**Firmware — add:**
- CFG characteristic + JSON read/write (`ble_sync.cpp`), `bleSyncSetDeviceId`,
  `bleSyncUpdatePending`, CRC-32 helper + cache, binary DATA framing + windowed
  ACK, `ABORT`/`WIN`/`GET offset` handling, advertising layout v2, security fix
  (remove `!isBonded()` gate; bond-window enforcement in auth-complete;
  `deleteAllBonds()` on factory reset).

**iOS — remove assumptions:**
- Manual-only sync UI copy; the old 3-byte mfg-data pairing-flag offset
  (index 2 → now index 3); the byte-count "finishGet" heuristic.

**iOS — add:**
- `UIBackgroundModes:[bluetooth-central]`, background-mode capability; CB restore
  identifier + `willRestoreState`; standing no-timeout connect + svc-filtered
  background scan; background `URLSession` upload path + delegate + launch-event
  handler; per-command timeouts; reliable framed receiver + resume; CFG read +
  device-id editor; "awaiting ACK" persistence; SHA-256 + `X-Content-Sha256`.

**iOS — config keys (UserDefaults):** keep `server_url`, `paired_device_id`,
`paired_device_name`; add `paired_device_cfg_id`, `awaiting_ack` (array of
`{name,deviceId}`). Keychain unchanged.

**Server — add:**
- Unique index `(device_id, sha256)`; dedup lookup before insert → 200
  `duplicate:true`; optional `X-Content-Sha256` validation (`400 sha_mismatch`);
  `duplicate` field in success response.

---

## G. Test & Acceptance Plan

### G.1 Firmware (independent, against contract)

- **Adv format:** use `nRF Connect` (or a host BLE script) to confirm: SVC UUID
  in primary packet; mfg-data `[FF FF 02 <flags> <count> <batt>]`; PENDING bit
  toggles after record/ACK; adv interval fast when pending.
- **Security:** unbonded central writing CTRL → Insufficient Authentication; pair
  during window → bond persists across reboot; **reconnect after reboot using a
  fresh RPA succeeds and can LIST** (bug #1 regression test). New connection
  while pairing window closed: existing bond OK, brand-new bond rejected.
- **Transfer:** scripted central performs `GET name 0`, validates frame
  `{seq,len}` monotonic, sends `ACKW`, induces a gap (drop a notify) → device
  retransmits on `NAK`; full-file CRC matches `GS` value; `GET name <offset>`
  resumes correctly.
- **Config:** read CFG → JSON with device_id/fw/proto; write `{"device_id":"x"}`
  → persists, advertised name changes, survives reboot.
- **Acceptance:** all of the above pass with no `delay()`-based pacing in the
  send path; main loop never blocked > ~20 ms; no SD access during Recording.

### G.2 iOS (independent, against contract)

- **Mock device:** a small CoreBluetooth-peripheral or host script implementing
  Section B. Verify: per-command timeouts fire (kill the mock mid-LIST/mid-GET →
  continuation fails within budget, no hang — bug #2); `subscribedCount` only
  counts successful subscribes; `deviceName`/cfg-id never defaults to `"prawnd"`
  when a name/CFG is available.
- **Reliable receiver:** mock drops/reorders DATA frames → app NAKs and recovers;
  CRC mismatch → app retries/resumes; partial persisted and resumed across an
  app relaunch.
- **Background:** with restore identifier set, force-relaunch the app process
  (Xcode "Wait for executable to launch" + toggle BT / use a real device) and
  confirm `willRestoreState` re-adopts the peripheral; standing connect completes
  and triggers `autoSyncIfPending`; background `URLSession` upload completes after
  suspension and the launch-event handler runs; ACK sent in a later wake.
- **Acceptance:** a recorded file uploads with the app backgrounded and never
  foregrounded after the initial install/launch; duplicate POST returns
  `duplicate:true` and still ACKs.

### G.3 Server (independent)

- POST same WAV twice (same device) → second returns 200 `duplicate:true`, no
  second file/row. Different device, same bytes → two rows (per-device dedup).
- `X-Content-Sha256` mismatch → 400 `sha_mismatch`. Absent header → still works.
- Auth unchanged (401 paths intact).

### G.4 Integration

1. Record on device → confirm PENDING adv bit set.
2. Phone backgrounded → wakes, drains, uploads, ACKs → device PENDING bit clears,
   file in `/uploaded`, server has exactly one recording.
3. Pull the phone out of range mid-GET → device keeps file; phone resumes on
   reconnect; final CRC ok; single server record.
4. Kill app, record again, relaunch once → subsequent records auto-sync in
   background.

**Top-level acceptance criteria:**
- Zero WiFi code/symbols remain in firmware; firmware contains no server URL/API
  key.
- A completed recording reaches the server **with no user interaction** while the
  app is backgrounded (after one prior launch).
- Bug #1: bonded iOS phones reconnect after RPA rotation and reboot — verified.
- Bug #2: no operation can hang indefinitely; all have timeouts; deviceName/cfg
  filing correct; `subscribedCount` correct.
- Bug #3: file transfer survives dropped notifications and verifies via CRC;
  short/stalled transfers self-heal via NAK/resume.
- No duplicate server-side recordings under retries.

---

## H. Open Questions / Risks (with recommendations)

1. **True "app fully closed" background wake.** iOS only relaunches a terminated
   app for Core Bluetooth events if state restoration is configured **and** the
   app has been launched at least once since install/force-quit. *Recommendation:*
   accept this; document the "launch once" requirement; rely on standing connect
   + state restoration. There is no way around the post-force-quit gap.

2. **Large WAVs vs short wake windows.** A multi-MB file may not finish in one
   background BLE window. *Recommendation (chosen):* resumable transfer (B.5) so
   progress is never lost; background `URLSession` for the HTTP leg so the network
   transfer doesn't compete for the wake window. Additionally, **cap recording
   length** or **roll over to a new file every ~60 s** of audio so individual
   files stay ≤ ~2 MB and usually drain in one window. *Default:* keep current
   record-until-pressed behaviour but flag rollover as a fast-follow.

3. **Windowed-ACK vs indications.** Indications give per-PDU acknowledgement but
   are slow (one outstanding at a time) and CoreBluetooth gives the app no extra
   reliability over notifications in practice. *Recommendation (chosen):* keep
   NOTIFY for throughput + app-level windowed ACK/NAK + per-file CRC. Best
   throughput/reliability balance; fully specified in B.5.

4. **CRC-32 vs SHA-256 on device.** SHA-256 over multi-MB on a 1 MHz SD + RISC-V
   core is slow. *Recommendation:* CRC-32 for the BLE hop (cheap, cached), and the
   phone computes SHA-256 only once on the assembled file for the server
   idempotency header. Two different integrity layers, each cheap where it runs.

5. **Server dedup response code.** 200+`duplicate:true` vs 409. *Recommendation:*
   200+`duplicate:true` (simplest client logic); client treats both as ACK-worthy
   regardless, so either is safe.

6. **Pairing-window enforcement location.** Enforcing "new bonds only during the
   window" in `onAuthenticationComplete` (delete-and-disconnect a new bond formed
   outside the window) is slightly racy but correct. *Recommendation:* implement
   it there and add a regression test (G.1). Reconnections of existing bonds are
   never affected.

7. **Power/sleep.** This spec does not add deep-sleep. Idle advertising at 1 s is
   the main idle cost. *Recommendation:* leave sleep out of scope; revisit after
   measuring real idle draw with WiFi removed (WiFi removal alone should cut idle
   current substantially).

8. **Proto version negotiation / old firmware.** *Recommendation:* app refuses
   `proto < 2` with an "update firmware" message rather than maintaining the
   legacy text-streaming path — simpler and the install base is tiny (beta).
