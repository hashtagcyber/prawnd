// Full carrier-board bench test: both INMP441 mics, SD card, and the D1 LED.
// No BLE, no button. Sequence:
//
//   1. LED blink 3x (LED path test).
//   2. SD test: mount at 1 MHz (same as real firmware), print card info,
//      write /boardtest.txt, read it back, verify. FAIL -> 3 triple-blinks,
//      then carry on to the mic test anyway.
//   3. Record 10 s of stereo 16 kHz WAV to /boardtest.wav — the end-to-end
//      test (I2S capture -> SD write, same conversion as the real firmware).
//      LED is solid while recording, like the real firmware.
//   4. Forever after: L/R serial level meter, LED lights while either mic
//      hears sound. 'r' on serial = software reboot.
//
//   pio run -e boardtest -t upload
//   pio device monitor -p /dev/cu.usbmodem1101 -b 115200
//
// Pull the card afterwards and play boardtest.wav on the computer — clean
// stereo audio with L=Mic1 and R=Mic2 proves the whole audio+storage chain.
#include <Arduino.h>
#include <driver/i2s_std.h>
#include <SD.h>
#include <SPI.h>
#include "pins.h"

static i2s_chan_handle_t rx = nullptr;
static const uint32_t SAMPLE_RATE = 16000;
static const uint32_t RECORD_SECONDS = 10;
static const int GAIN = 4;              // same empirical gain as audio.cpp
static const int32_t LED_THRESH = 600;  // RMS (16-bit scale) to light the LED

// ---------------------------------------------------------------- LED helpers
static void blink(int times, int ms) {
  for (int i = 0; i < times; i++) {
    digitalWrite(PIN_REC_LED, HIGH); delay(ms);
    digitalWrite(PIN_REC_LED, LOW);  delay(ms);
  }
}

static void sdFailPattern() {  // 3 triple-blinks
  for (int i = 0; i < 3; i++) { blink(3, 100); delay(400); }
}

// ---------------------------------------------------------------- I2S (as audio.cpp)
static bool i2sInit() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num  = 8;     // big buffers so SD write latency can't
  chan_cfg.dma_frame_num = 1024;  // drop samples (~512 ms of headroom)
  if (i2s_new_channel(&chan_cfg, nullptr, &rx) != ESP_OK) return false;

  i2s_std_config_t cfg = {};
  cfg.clk_cfg.sample_rate_hz = SAMPLE_RATE;
  cfg.clk_cfg.clk_src        = I2S_CLK_SRC_DEFAULT;
  cfg.clk_cfg.mclk_multiple  = I2S_MCLK_MULTIPLE_256;

  cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
  cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
  cfg.slot_cfg.slot_mode      = I2S_SLOT_MODE_STEREO;
  cfg.slot_cfg.slot_mask      = I2S_STD_SLOT_BOTH;
  cfg.slot_cfg.ws_width       = I2S_DATA_BIT_WIDTH_32BIT;
  cfg.slot_cfg.ws_pol         = false;
  cfg.slot_cfg.bit_shift      = true;   // Philips framing

  cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  cfg.gpio_cfg.bclk = (gpio_num_t)PIN_I2S_BCLK;
  cfg.gpio_cfg.ws   = (gpio_num_t)PIN_I2S_WS;
  cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  cfg.gpio_cfg.din  = (gpio_num_t)PIN_I2S_DIN;

  if (i2s_channel_init_std_mode(rx, &cfg) != ESP_OK) return false;
  if (i2s_channel_enable(rx) != ESP_OK) return false;
  return true;
}

static void i2sDropSettle() {  // discard the mics' power-up ramp (~50 ms)
  int32_t junk[256]; size_t got;
  for (size_t total = 0; total < 1600; total += got / sizeof(int32_t))
    if (i2s_channel_read(rx, junk, sizeof(junk), &got, pdMS_TO_TICKS(100)) != ESP_OK) break;
}

// Same sample conversion as audio.cpp minus the voice-band filters.
static inline int16_t convert(int32_t raw) {
  int32_t s = raw >> 14;
  s >>= 2;
  s *= GAIN;
  if (s >  32767) s =  32767;
  if (s < -32768) s = -32768;
  return (int16_t)s;
}

// ---------------------------------------------------------------- SD test
static bool sdTest() {
  Serial.println("[sd] mounting (1 MHz, CS/SCK/MISO/MOSI = GPIO 21/19/20/18)...");
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS, SPI, 1000000)) {
    Serial.println("[sd] FAIL: SD.begin() — no card response. Check pinout/card/format (must be FAT32).");
    SPI.end();
    return false;
  }
  const char *types[] = {"NONE", "MMC", "SDSC", "SDHC", "UNKNOWN"};
  uint8_t t = SD.cardType();
  Serial.printf("[sd] mounted: type=%s size=%llu MB\n",
                types[t < 5 ? t : 4], SD.cardSize() / (1024ULL * 1024ULL));

  const char *msg = "prawnd boardtest 2026-09-09";
  File f = SD.open("/boardtest.txt", FILE_WRITE);
  if (!f) { Serial.println("[sd] FAIL: can't open /boardtest.txt for write"); return false; }
  f.print(msg);
  f.close();

  f = SD.open("/boardtest.txt", FILE_READ);
  if (!f) { Serial.println("[sd] FAIL: can't reopen /boardtest.txt"); return false; }
  String back = f.readString();
  f.close();
  if (back != msg) {
    Serial.printf("[sd] FAIL: read-back mismatch (got \"%s\")\n", back.c_str());
    return false;
  }
  Serial.println("[sd] PASS: write + read-back verified");
  return true;
}

// ---------------------------------------------------------------- WAV record
static void wavHeader(uint8_t *h, uint32_t dataBytes) {
  const uint16_t ch = 2, bits = 16;
  const uint32_t byteRate = SAMPLE_RATE * ch * bits / 8;
  memcpy(h, "RIFF", 4);  *(uint32_t *)(h + 4) = 36 + dataBytes;
  memcpy(h + 8, "WAVEfmt ", 8);
  *(uint32_t *)(h + 16) = 16;                 // fmt chunk size
  *(uint16_t *)(h + 20) = 1;                  // PCM
  *(uint16_t *)(h + 22) = ch;
  *(uint32_t *)(h + 24) = SAMPLE_RATE;
  *(uint32_t *)(h + 28) = byteRate;
  *(uint16_t *)(h + 32) = ch * bits / 8;      // block align
  *(uint16_t *)(h + 34) = bits;
  memcpy(h + 36, "data", 4); *(uint32_t *)(h + 40) = dataBytes;
}

static bool recordWav() {
  const uint32_t totalSamples = SAMPLE_RATE * 2 * RECORD_SECONDS;  // stereo int16
  const uint32_t dataBytes = totalSamples * 2;

  File f = SD.open("/boardtest.wav", FILE_WRITE);
  if (!f) { Serial.println("[rec] FAIL: can't open /boardtest.wav"); return false; }
  uint8_t hdr[44];
  wavHeader(hdr, dataBytes);
  if (f.write(hdr, sizeof(hdr)) != sizeof(hdr)) { f.close(); return false; }

  Serial.printf("[rec] recording %lu s stereo to /boardtest.wav — make some noise!\n",
                (unsigned long)RECORD_SECONDS);
  digitalWrite(PIN_REC_LED, HIGH);  // solid while recording, like the real fw

  static int32_t raw[512];
  static int16_t out[512];
  uint32_t written = 0, lastTick = 0;
  bool ok = true;
  while (written < totalSamples) {
    size_t got_bytes = 0;
    if (i2s_channel_read(rx, raw, sizeof(raw), &got_bytes, pdMS_TO_TICKS(200)) != ESP_OK) {
      Serial.println("[rec] FAIL: i2s read timeout mid-recording");
      ok = false; break;
    }
    size_t n = got_bytes / sizeof(int32_t);
    if (n > totalSamples - written) n = totalSamples - written;
    for (size_t i = 0; i < n; i++) out[i] = convert(raw[i]);
    if (f.write((const uint8_t *)out, n * 2) != n * 2) {
      Serial.println("[rec] FAIL: short SD write mid-recording");
      ok = false; break;
    }
    written += n;
    uint32_t sec = written / (SAMPLE_RATE * 2);
    if (sec != lastTick) { lastTick = sec; Serial.printf("[rec] %lu/%lu s\n", (unsigned long)sec, (unsigned long)RECORD_SECONDS); }
  }
  digitalWrite(PIN_REC_LED, LOW);

  if (ok && written < totalSamples) ok = false;
  if (!ok && written != totalSamples) {   // fix header for the shorter file
    wavHeader(hdr, written * 2);
    f.seek(0);
    f.write(hdr, sizeof(hdr));
  }
  f.close();
  File chk = SD.open("/boardtest.wav", FILE_READ);
  if (chk) {
    Serial.printf("[rec] %s: /boardtest.wav on card, %lu bytes (expected %lu)\n",
                  ok ? "PASS" : "PARTIAL", (unsigned long)chk.size(),
                  (unsigned long)(44 + dataBytes));
    chk.close();
  }
  return ok;
}

// ---------------------------------------------------------------- level meter
static void barStr(char *out, int32_t rms) {
  int len = 0;
  for (int32_t v = 8; v < rms && len < 20; v *= 2) len++;
  for (int i = 0; i < 20; i++) out[i] = (i < len) ? '#' : ' ';
  out[20] = '\0';
}

void setup() {
  pinMode(PIN_REC_LED, OUTPUT);
  digitalWrite(PIN_REC_LED, LOW);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);

  Serial.println();
  Serial.println("[boardtest] Prawnd carrier full bench test: LED + SD + mics");

  Serial.println("[boardtest] 1/4 LED blink 3x...");
  blink(3, 300);

  Serial.println("[boardtest] 2/4 SD card...");
  bool sdOk = sdTest();
  if (!sdOk) sdFailPattern();

  Serial.println("[boardtest] 3/4 I2S mics...");
  if (!i2sInit()) {
    Serial.println("[boardtest] I2S init FAILED — rapid-blinking forever");
    while (true) blink(1, 80);
  }
  i2sDropSettle();

  if (sdOk) {
    Serial.println("[boardtest] 4/4 recording test WAV...");
    recordWav();
  } else {
    Serial.println("[boardtest] 4/4 SKIPPED (no SD) — level meter only");
  }

  Serial.println("[boardtest] live meter: L = Mic1 (L/R->GND), R = Mic2 (L/R->3V3).");
  Serial.println("[boardtest] LED lights while either mic hears sound. 'r' = reboot.");
}

void loop() {
  if (Serial.available() && Serial.read() == 'r') {
    Serial.println("[boardtest] rebooting...");
    Serial.flush();
    ESP.restart();
  }

  static int32_t buf[512];
  int64_t sumSq[2] = {0, 0};
  int32_t peak[2] = {0, 0};
  int32_t rawFirst[2] = {0, 0};
  bool flat[2] = {true, true};
  size_t cnt[2] = {0, 0};
  bool first = true;

  for (int reads = 0; reads < 8; reads++) {   // ~250 ms window
    size_t got_bytes = 0;
    if (i2s_channel_read(rx, buf, sizeof(buf), &got_bytes, pdMS_TO_TICKS(200)) != ESP_OK) {
      Serial.println("[boardtest] i2s read TIMEOUT — clock lines dead?");
      return;
    }
    size_t n = got_bytes / sizeof(int32_t);
    for (size_t i = 0; i < n; i++) {
      int k = i & 1;                     // even = L slot, odd = R slot
      if (first && i < 2) rawFirst[k] = buf[i];
      if (buf[i] != rawFirst[k]) flat[k] = false;
      int32_t s = buf[i] >> 16;
      if (s < 0) s = -s;
      sumSq[k] += (int64_t)s * s;
      if (s > peak[k]) peak[k] = s;
      cnt[k]++;
    }
    first = false;
  }

  int32_t rms[2];
  char bars[2][21];
  for (int k = 0; k < 2; k++) {
    rms[k] = cnt[k] ? (int32_t)sqrt((double)sumSq[k] / cnt[k]) : 0;
    barStr(bars[k], rms[k]);
  }
  digitalWrite(PIN_REC_LED, (rms[0] > LED_THRESH || rms[1] > LED_THRESH) ? HIGH : LOW);

  Serial.printf("L|%s| rms %5ld peak %5ld%s   R|%s| rms %5ld peak %5ld%s\n",
                bars[0], (long)rms[0], (long)peak[0], flat[0] ? " FLAT!" : "",
                bars[1], (long)rms[1], (long)peak[1], flat[1] ? " FLAT!" : "");
}
