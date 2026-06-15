# Prawnd

An ESP32-based, Plaud-style voice recorder. Press a button to start/stop recording;
the device captures 16 kHz stereo audio to an SD card and POSTs each finished WAV
to a webserver you configure on-device.

- **Hardware** — Seeed XIAO ESP32C6, 2× INMP441 I²S mics (stereo), SPI microSD adapter, push-button. Wiring diagrams live in [`hardware/wiring.md`](hardware/wiring.md).
- **Firmware** — Arduino-ESP32 via PlatformIO. Source in [`firmware/`](firmware/).
- **Webservice** — Node.js + Fastify, SQLite-indexed, Dockerized. Source in [`server/`](server/).

> [!CAUTION]
> ## 🔓 HTTPS uploads use `setInsecure()` — TLS certificate validation is OFF
>
> When the configured Upload URL is `https://…`, the firmware uploads with
> `WiFiClientSecure::setInsecure()` (`firmware/src/uploader.cpp`), which **does
> the TLS handshake but does NOT validate the server's certificate**. The
> connection is encrypted, but there is **no protection against a
> man-in-the-middle**: anyone who can intercept the device's traffic can present
> a forged certificate, decrypt the upload, and **steal the API key** (which the
> device sends as `Authorization: Bearer …`).
>
> This is a **beta convenience only.** Before any non-hobby/production use, pin a
> CA bundle (e.g. ISRG Root X1) via `setCACert()` and remove `setInsecure()`, and
> treat each device's API key as compromised if its network is ever untrusted.
> Rotate keys in the web UI if in doubt.

## Quick start

```bash
# 1. Bring the server up locally
cd server
cp .env.example .env            # then edit PRAWND_API_KEY
docker compose up -d --build
curl -s http://localhost:8080/healthz   # {"ok":true}

# 2. Build the firmware
cd ../firmware
pio run                                  # first run downloads toolchains (slow)

# 3. Flash it (with the XIAO plugged in via USB-C)
pio run -t upload
pio device monitor                       # 115200 baud
```

On first boot the device has no WiFi configured, so it brings up an AP named
**`Prawnd-XXXX`** (password `prawnd1234`). Connect, browse to `http://192.168.4.1`,
fill in WiFi creds + upload URL + API key, hit save. The device reboots into STA
mode and is ready to record.

## Server

### Prereqs
- Docker / Docker Compose (Rancher Desktop, OrbStack, or Docker Desktop all fine)

### Build & run
```bash
cd server
cp .env.example .env       # set PRAWND_API_KEY
docker compose up -d --build
docker compose logs -f prawnd
```

The container exposes port `8080`, persists data under `./data/` on the host
(SQLite at `data/prawnd.sqlite`, WAVs under `data/audio/<device_id>/<yyyy>/<mm>/`).

### Browser UI

Visit `http://localhost:8080/` for a built-in single-page UI: enter your API key
once (cached in `localStorage`), filter by device id, and play back any recording
inline. Audio elements stream from `/recordings/:id/file?api_key=...` directly.
**Caveat**: the API key ends up in `<audio src>` attributes, so treat the URL as
secret on the LAN — don't share screenshots of the DOM.

### Endpoints

| Method | Path | Auth | Purpose |
|---|---|---|---|
| `GET`  | `/` | no | Built-in HTML UI. |
| `POST` | `/upload` | yes | Upload a WAV body. Headers: `Authorization: Bearer <key>` (or `X-API-Key: <key>`), `Content-Type: audio/wav`, `X-Device-Id: <id>`, `X-Client-Timestamp: <ms>` (optional). |
| `GET`  | `/recordings?limit=&cursor=&device_id=` | yes | Cursor-paginated list, newest first. |
| `GET`  | `/recordings/:id` | yes | JSON metadata. |
| `GET`  | `/recordings/:id/file` | yes | Raw `audio/wav` download. Auth accepts `?api_key=` query for `<audio>` playback. |
| `GET`  | `/healthz` | no | Liveness. |

API key auth accepts any of: `Authorization: Bearer <key>` header, `X-API-Key: <key>` header, or `?api_key=<key>` query param.

### Manual upload (smoke test without firmware)
```bash
curl -fsS -X POST \
  -H "Authorization: Bearer $PRAWND_API_KEY" \
  -H "Content-Type: audio/wav" \
  -H "X-Device-Id: bench" \
  --data-binary @some.wav \
  http://localhost:8080/upload
curl -fsS -H "Authorization: Bearer $PRAWND_API_KEY" http://localhost:8080/recordings | jq
```

### Stop / clean
```bash
docker compose down            # stop
rm -rf data && mkdir data      # wipe all recordings + db
```

## Firmware

### Prereqs
- [PlatformIO Core](https://platformio.org/install/cli) (`brew install platformio` on macOS)
- A USB-C cable that carries data
- A microSD card formatted **FAT32**

### Build
```bash
cd firmware
pio run
```
First run pulls the `espressif32` platform, the Arduino-ESP32 framework, the
toolchain, and the listed libs — expect several minutes on a fresh machine.
Subsequent builds take seconds. The flashable binary lands at
`.pio/build/xiao_esp32c6/firmware.bin`.

### Flash
Plug the XIAO in via USB-C. The C6 enters bootloader automatically on most
ports; if not, hold the **BOOT** button, tap **RESET**, then release BOOT.

The compiled artifact is `.pio/build/xiao_esp32c6/firmware.bin` (~1.1 MB).
```bash
pio run -t upload                # build + flash
pio run -t upload -t monitor     # flash + open serial monitor
```
Set the port explicitly if PlatformIO can't auto-detect:
```bash
pio run -t upload --upload-port /dev/cu.usbmodem<TAB>
```

### Serial monitor
```bash
pio device monitor               # 115200 baud (configured in platformio.ini)
```

### Optional: battery-monitoring addon

A second PlatformIO env, **`xiao_esp32c6_batt`**, compiles in an optional
MAX17048 LiPo fuel-gauge driver. The base `xiao_esp32c6` build is unchanged — the
addon is purely a compile-time flag (`-DENABLE_BATTERY`), so the standard
USB-powered firmware carries no battery code.

```bash
pio run -e xiao_esp32c6_batt              # build the battery variant
pio run -e xiao_esp32c6_batt -t upload    # build + flash it
```

Wire a MAX17048 breakout to the XIAO's I²C pins (`D4`=SDA, `D5`=SCL) and the BAT
pads — see [`hardware/wiring.md`](hardware/wiring.md#battery-fuel-gauge--max17048-optional-addon).
When a gauge is detected, `GET /status` gains `batt_pct` and `batt_mv` fields and
the config page shows the charge level; with no gauge fitted it reports
`battery: n/a` and behaves identically to the base firmware. The artifact lands
at `.pio/build/xiao_esp32c6_batt/firmware.bin`.

### First-time WiFi + upload setup
1. After flash, the device has no NVS config → it boots into AP mode.
2. From a phone or laptop, join the WiFi network **`Prawnd-XXXX`** (password `prawnd1234`, where `XXXX` is the last 4 hex of the MAC).
3. Open `http://192.168.4.1`.
4. Enter:
   - WiFi SSID + password (your home/office WiFi)
   - Upload URL — either:
     - **Local server:** `http://<your-mac-IP>:8080/upload` (plain HTTP), or
     - **Hosted beta:** `https://prawnd.dev.cobl.io/upload` (HTTPS — see the
       certificate-validation caution at the top of this README).
   - API key:
     - Local server: must match the server's `PRAWND_API_KEY`.
     - Hosted beta: log in at `https://prawnd.dev.cobl.io` with cobl.io and
       generate an **upload key** under *API keys*.
   - Device id (optional; auto-derived from MAC)
5. Hit **Save and reboot**. The device reconnects in STA mode.

To find the device's IP after reboot, watch the serial monitor or check your
router's DHCP list. `GET http://<device-ip>/status` returns its current state.

### Troubleshooting the first build

The `firmware/platformio.ini` deliberately uses the **pioarduino** platform fork
(`https://github.com/pioarduino/platform-espressif32/releases/...`) because the
official `platform-espressif32` still bundles Arduino-ESP32 v2.x, which has no
ESP32-C6 support. Two snags you may hit on a fresh machine:

- **`Could not find a version that satisfies the requirement cryptography` from pip** — your global pip is pointing at a local mirror that isn't serving. Override for the one command: `PIP_INDEX_URL=https://pypi.org/simple/ pio run`.
- **`FileNotFoundError: 'package-postinstall.py'` from the pioarduino esptool zip** — the v4.8.6 release omits the post-install script that PlatformIO tries to run. Stub it once: `touch ~/.platformio/packages/tool-esptoolpy/package-postinstall.py`, then re-run `pio run`.

### Recording flow
- **Short-press button** — start recording (LED on board lights, serial logs path)
- **Short-press again** — stop, patch WAV header, queue for upload, POST to server
- **Long-press ≥ 5 s** — factory reset (wipes NVS, reboots into AP mode)

Pending uploads (WiFi down, server unreachable) land in `/pending/` on the SD
card and retry once a minute while the device is idle and connected. Successfully
uploaded files move to `/uploaded/`.

## End-to-end verification

1. `cd server && docker compose up -d` — confirm `/healthz` returns 200.
2. `cd ../firmware && pio run -t upload && pio device monitor` — flash and watch boot logs.
3. Join the AP, configure with the upload URL pointing at your machine's LAN IP (NOT `localhost` — that's the device's own loopback).
4. Press the button, speak for a few seconds, press again.
5. Watch the serial log for `Upload .../ -> 200`.
6. `curl -fsS -H "Authorization: Bearer $PRAWND_API_KEY" http://localhost:8080/recordings | jq` — your recording shows up at the top.
7. Pull the file: `curl -fsS -H "Authorization: Bearer $PRAWND_API_KEY" http://localhost:8080/recordings/<id>/file -o out.wav` and play in VLC/Audacity — both L and R channels should be populated.

## Project layout

```
prawnd/
├── README.md
├── hardware.md                # high-level spec
├── hardware/
│   └── wiring.md              # per-device wiring diagrams + pin map
├── firmware/
│   ├── platformio.ini
│   ├── include/pins.h
│   └── src/                   # main, audio, wav, sd, wifi, portal, uploader, button, config,
│                              #   battery (optional MAX17048 addon, -DENABLE_BATTERY)
└── server/
    ├── Dockerfile
    ├── docker-compose.yml
    ├── package.json
    ├── .env.example
    └── src/                   # server.js, db.js, auth.js, wav.js,
                               #   routes/ (upload, recordings, health, ui)
```

## Hardware Links
I purchased the following parts on Amazon:
- https://a.co/d/0gR9T4nS
- https://a.co/d/04cU7Vte
- https://a.co/d/02JYXZO7
- https://a.co/d/06rkL2Dp