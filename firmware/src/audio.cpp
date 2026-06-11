#include "audio.h"
#include "pins.h"
#include <driver/i2s_std.h>

static i2s_chan_handle_t rx_handle = nullptr;
static const uint32_t SAMPLE_RATE = 16000;
static const int GAIN = 4;

// --- 2nd-order Butterworth biquads, direct-form II transposed -------------
// One HPF @ 100 Hz and one LPF @ 3.5 kHz per channel.
// Coefficients are derived once (offline) for fs=16000 Hz, Q=1/sqrt(2):
//
//   HPF 100 Hz:  b = [ 0.97264, -1.94527,  0.97264], a = [1, -1.94454, 0.94600]
//   LPF 3500 Hz: b = [ 0.31729,  0.63458,  0.31729], a = [1,  0.06443, 0.30551]
//
// Stored in Q15 (×32768). |b1|, |a1| can exceed 1, so coefficients live in
// int32 and the multiply-accumulate widens to int64 to avoid overflow.
//
// Each biquad gives -12 dB/octave roll-off vs the previous 1-pole's -6 dB/oct,
// so the same cutoff knocks the stop-band down ~4× harder for almost no CPU.
struct Biquad {
  int32_t b0, b1, b2, a1, a2;
  int32_t z1, z2;  // state (direct-form II transposed)
};

// y[n]  = b0*x + z1
// z1[n] = b1*x - a1*y + z2
// z2[n] = b2*x - a2*y
static inline int16_t biquadStep(Biquad &q, int32_t x) {
  int64_t y64 = ((int64_t)q.b0 * x + q.z1) >> 15;
  int32_t y = (int32_t)y64;
  if (y >  32767) y =  32767;
  if (y < -32768) y = -32768;
  q.z1 = (int32_t)((((int64_t)q.b1 * x) - ((int64_t)q.a1 * y) + ((int64_t)q.z2 << 15)) >> 15);
  q.z2 = (int32_t)((((int64_t)q.b2 * x) - ((int64_t)q.a2 * y)) >> 15);
  return (int16_t)y;
}

// HPF 100 Hz coefficients ×32768
static constexpr int32_t HPF_B0 =  31874;
static constexpr int32_t HPF_B1 = -63747;
static constexpr int32_t HPF_B2 =  31874;
static constexpr int32_t HPF_A1 = -63723;
static constexpr int32_t HPF_A2 =  30999;

// LPF 3500 Hz coefficients ×32768
static constexpr int32_t LPF_B0 =  10396;
static constexpr int32_t LPF_B1 =  20791;
static constexpr int32_t LPF_B2 =  10396;
static constexpr int32_t LPF_A1 =   2111;
static constexpr int32_t LPF_A2 =  10010;

static Biquad hpf_L = { HPF_B0, HPF_B1, HPF_B2, HPF_A1, HPF_A2, 0, 0 };
static Biquad hpf_R = { HPF_B0, HPF_B1, HPF_B2, HPF_A1, HPF_A2, 0, 0 };
static Biquad lpf_L = { LPF_B0, LPF_B1, LPF_B2, LPF_A1, LPF_A2, 0, 0 };
static Biquad lpf_R = { LPF_B0, LPF_B1, LPF_B2, LPF_A1, LPF_A2, 0, 0 };

static inline void biquadReset(Biquad &q) { q.z1 = q.z2 = 0; }

bool audioBegin() {
  // Reset filter state on each recording start so we don't pop on the first sample.
  biquadReset(hpf_L); biquadReset(hpf_R);
  biquadReset(lpf_L); biquadReset(lpf_R);

  // Larger DMA buffer than the default 6×240 (90 ms). 8×1024 frames = ~512 ms
  // of buffering at 16 kHz stereo, which lets the SD writer task be late
  // without dropping samples or modulating the I2S cadence.
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num  = 8;
  chan_cfg.dma_frame_num = 1024;
  if (i2s_new_channel(&chan_cfg, nullptr, &rx_handle) != ESP_OK) return false;

  // Set struct fields explicitly (without the IDF macros): the macros emit C99
  // designators out of declaration order, which is a hard error under C++,
  // and the SLOT macro references HW-v1-only fields that don't exist on C6.
  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg.sample_rate_hz = SAMPLE_RATE;
  std_cfg.clk_cfg.clk_src        = I2S_CLK_SRC_DEFAULT;
  std_cfg.clk_cfg.mclk_multiple  = I2S_MCLK_MULTIPLE_256;

  std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
  std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
  std_cfg.slot_cfg.slot_mode      = I2S_SLOT_MODE_STEREO;
  std_cfg.slot_cfg.slot_mask      = I2S_STD_SLOT_BOTH;
  std_cfg.slot_cfg.ws_width       = I2S_DATA_BIT_WIDTH_32BIT;
  std_cfg.slot_cfg.ws_pol         = false;
  std_cfg.slot_cfg.bit_shift      = true;   // Philips framing

  std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.bclk = (gpio_num_t)PIN_I2S_BCLK;
  std_cfg.gpio_cfg.ws   = (gpio_num_t)PIN_I2S_WS;
  std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.din  = (gpio_num_t)PIN_I2S_DIN;

  if (i2s_channel_init_std_mode(rx_handle, &std_cfg) != ESP_OK) {
    i2s_del_channel(rx_handle);
    rx_handle = nullptr;
    return false;
  }
  if (i2s_channel_enable(rx_handle) != ESP_OK) {
    i2s_del_channel(rx_handle);
    rx_handle = nullptr;
    return false;
  }
  return true;
}

void audioEnd() {
  if (rx_handle) {
    i2s_channel_disable(rx_handle);
    i2s_del_channel(rx_handle);
    rx_handle = nullptr;
  }
}

void audioDropSettle() {
  int32_t buf[256];
  size_t got = 0;
  size_t total = 0;
  while (total < 1600) {  // ~50 ms at 16 kHz stereo
    if (i2s_channel_read(rx_handle, buf, sizeof(buf), &got, pdMS_TO_TICKS(100)) != ESP_OK) break;
    total += got / sizeof(int32_t);
  }
}

size_t audioReadFrames(int16_t *out, size_t maxOutInt16) {
  static int32_t buf32[512];
  size_t want_count = maxOutInt16 < 512 ? maxOutInt16 : 512;
  size_t want_bytes = want_count * sizeof(int32_t);
  size_t got_bytes = 0;
  if (i2s_channel_read(rx_handle, buf32, want_bytes, &got_bytes, pdMS_TO_TICKS(100)) != ESP_OK) return 0;
  size_t n = got_bytes / sizeof(int32_t);
  for (size_t i = 0; i < n; i++) {
    int32_t s = buf32[i] >> 14;   // 18-bit-ish signed (works for SPH0645 and INMP441)
    s >>= 2;                       // down to 16-bit
    s *= GAIN;                     // empirical, tune at bring-up
    if (s >  32767) s =  32767;
    if (s < -32768) s = -32768;
    // Stereo interleave: even = L, odd = R. Cascade HPF→LPF biquads per ch.
    int16_t y;
    if (i & 1) {
      int16_t hp = biquadStep(hpf_R, s);
      y = biquadStep(lpf_R, hp);
    } else {
      int16_t hp = biquadStep(hpf_L, s);
      y = biquadStep(lpf_L, hp);
    }
    out[i] = y;
  }
  return n;
}
