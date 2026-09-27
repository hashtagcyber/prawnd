// Bench bring-up test: both INMP441 mics + recording LED only.
// No SD, no button, no BLE — for validating the carrier board mic wiring
// before the SD pinout question is sorted out.
//
//   pio run -e mictest -t upload
//   pio device monitor -p /dev/cu.usbmodem101 -b 115200
//
// What to expect:
//   - LED blinks 3× at boot (LED + resistor path test, independent of mics).
//   - Serial prints an L/R level meter ~4×/second.
//   - Talk or snap near a mic: its channel's bar should move and the LED
//     lights while sound is above the threshold.
//   - Mic1 (L/R pad strapped to GND) is the LEFT column; Mic2 (L/R → 3V3)
//     is RIGHT. If only one column ever moves, check the silent mic's
//     VDD/GND/L-R strap; if BOTH move identically, the L/R straps may be
//     tied to the same rail (both mics on one slot).
//   - "flat" flag = every raw sample in the window was identical (a real
//     INMP441 always has noise + DC offset) → that slot has no live mic:
//     dead SD line, missing power, or wrong L/R strap.
//   - Serial 'r' = software reboot (keeps native USB alive, same as main fw).
#include <Arduino.h>
#include <driver/i2s_std.h>
#include "pins.h"

static i2s_chan_handle_t rx = nullptr;
static const uint32_t SAMPLE_RATE = 16000;

// RMS (16-bit scale) above which the LED turns on. Room noise sits well
// below 100 after the >>16 scaling; speech at arm's length is thousands.
// (Was 600; halved for a more sensitive bench LED.)
static const int32_t LED_THRESH = 300;

static bool i2sInit() {
  // Same explicit struct init as audio.cpp: the IDF C99 designator macros
  // don't compile as C++, and the SLOT macro touches HW-v1-only fields.
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
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

static void blink(int times, int ms) {
  for (int i = 0; i < times; i++) {
    digitalWrite(PIN_REC_LED, HIGH); delay(ms);
    digitalWrite(PIN_REC_LED, LOW);  delay(ms);
  }
}

struct ChanStats {
  int64_t sumSq = 0;
  int32_t peak = 0;
  int32_t rawFirst = 0;
  bool    flat = true;   // every raw sample identical → no live mic
  size_t  n = 0;
};

static void bar(char *out, int32_t rms) {
  // 20-char log-ish bar: each char ≈ doubling from 8.
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
  while (!Serial && millis() - t0 < 3000) delay(10);  // USB-CDC host wait

  Serial.println();
  Serial.println("[mictest] Prawnd carrier bring-up: mics + LED");
  Serial.printf("[mictest] BCLK=GPIO%d WS=GPIO%d DIN=GPIO%d LED=GPIO%d\n",
                PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DIN, PIN_REC_LED);

  Serial.println("[mictest] LED blink test (3x)...");
  blink(3, 300);

  if (!i2sInit()) {
    Serial.println("[mictest] I2S init FAILED — rapid-blinking forever");
    while (true) blink(1, 80);
  }
  Serial.println("[mictest] I2S up. L = Mic1 (L/R->GND), R = Mic2 (L/R->3V3)");
  Serial.println("[mictest] Talk at each mic; LED lights while it hears you.");

  // Let the mics' outputs ramp/settle (~50 ms) before we trust levels.
  int32_t junk[256]; size_t got;
  for (size_t total = 0; total < 1600; total += got / sizeof(int32_t))
    if (i2s_channel_read(rx, junk, sizeof(junk), &got, pdMS_TO_TICKS(100)) != ESP_OK) break;
}

void loop() {
  if (Serial.available() && Serial.read() == 'r') {
    Serial.println("[mictest] rebooting...");
    Serial.flush();
    ESP.restart();
  }

  // ~250 ms window: 4096 stereo int32 samples = 2048 per channel.
  static int32_t buf[512];
  ChanStats ch[2];
  bool first = true;
  for (int reads = 0; reads < 8; reads++) {
    size_t got_bytes = 0;
    if (i2s_channel_read(rx, buf, sizeof(buf), &got_bytes, pdMS_TO_TICKS(200)) != ESP_OK) {
      Serial.println("[mictest] i2s read TIMEOUT — clock lines dead?");
      return;
    }
    size_t n = got_bytes / sizeof(int32_t);
    for (size_t i = 0; i < n; i++) {
      ChanStats &c = ch[i & 1];          // even = L slot, odd = R slot
      if (first && i < 2) { c.rawFirst = buf[i]; }
      if (buf[i] != ch[i & 1].rawFirst) c.flat = false;
      int32_t s = buf[i] >> 16;          // 32-bit slot → 16-bit-ish level
      if (s < 0) s = -s;
      c.sumSq += (int64_t)s * s;
      if (s > c.peak) c.peak = s;
      c.n++;
    }
    first = false;
  }

  // Raw word dump every ~2 s: 16 consecutive slot words (L,R,L,R...) in hex,
  // so a misframed / non-mic bit pattern can be recognised directly.
  static uint32_t lastDump = 0;
  if (millis() - lastDump > 2000) {
    lastDump = millis();
    Serial.print("[raw]");
    for (int i = 0; i < 16; i++) Serial.printf(" %08lx", (unsigned long)(uint32_t)buf[i]);
    Serial.println();
  }

  int32_t rms[2];
  char bars[2][21];
  for (int k = 0; k < 2; k++) {
    rms[k] = ch[k].n ? (int32_t)sqrt((double)ch[k].sumSq / ch[k].n) : 0;
    bar(bars[k], rms[k]);
  }

  digitalWrite(PIN_REC_LED, (rms[0] > LED_THRESH || rms[1] > LED_THRESH) ? HIGH : LOW);

  Serial.printf("L|%s| rms %5ld peak %5ld%s   R|%s| rms %5ld peak %5ld%s\n",
                bars[0], (long)rms[0], (long)ch[0].peak, ch[0].flat ? " FLAT!" : "",
                bars[1], (long)rms[1], (long)ch[1].peak, ch[1].flat ? " FLAT!" : "");
}
