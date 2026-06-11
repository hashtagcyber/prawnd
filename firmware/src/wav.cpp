#include "wav.h"
#include <SD.h>

static void writeLE32(uint8_t *p, uint32_t v) {
  p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
  p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
static void writeLE16(uint8_t *p, uint16_t v) {
  p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
}

void wavWritePlaceholderHeader(File &f, uint32_t sample_rate, uint16_t channels, uint16_t bits) {
  // Initialise the size fields to a "max signed int" sentinel rather than 0.
  // Most players (QuickTime, VLC, ffmpeg, Audacity) treat 0x7FFFFFFE as
  // "really big — just read until EOF", which is what we want. We still try
  // to patch the real sizes on stop via wavPatchSizes(), but if that fails
  // (e.g. SD bus is glitching) the file is still well-formed and playable.
  static constexpr uint32_t SENTINEL = 0x7FFFFFFE;
  uint8_t hdr[44] = {0};
  memcpy(hdr, "RIFF", 4);
  writeLE32(hdr + 4, SENTINEL);                                  // riff size
  memcpy(hdr + 8, "WAVE", 4);
  memcpy(hdr + 12, "fmt ", 4);
  writeLE32(hdr + 16, 16);                                       // fmt chunk size
  writeLE16(hdr + 20, 1);                                        // PCM
  writeLE16(hdr + 22, channels);
  writeLE32(hdr + 24, sample_rate);
  uint32_t byte_rate = sample_rate * channels * (bits / 8);
  writeLE32(hdr + 28, byte_rate);
  uint16_t block_align = channels * (bits / 8);
  writeLE16(hdr + 32, block_align);
  writeLE16(hdr + 34, bits);
  memcpy(hdr + 36, "data", 4);
  writeLE32(hdr + 40, SENTINEL);                                 // data size
  f.write(hdr, 44);
}

static bool patchOnce(const char *path, uint32_t total) {
  File f = SD.open(path, "r+");
  if (!f) return false;
  uint32_t data_size = total > 44 ? total - 44 : 0;
  uint32_t riff_size = total > 8  ? total - 8  : 0;
  uint8_t v[4];

  bool ok = true;
  if (!f.seek(4))                     ok = false;
  writeLE32(v, riff_size);
  if (ok && f.write(v, 4) != 4)       ok = false;
  if (ok && !f.seek(40))              ok = false;
  writeLE32(v, data_size);
  if (ok && f.write(v, 4) != 4)       ok = false;
  f.flush();
  f.close();
  return ok;
}

bool wavPatchSizes(const char *path, uint32_t total) {
  // Up to 3 attempts — each open/close cycle is bounded, so a single SD glitch
  // doesn't permanently kill the patch. 50 ms pause between tries lets the
  // card finish whatever internal programming it's busy with.
  for (int i = 0; i < 3; i++) {
    if (patchOnce(path, total)) return true;
    delay(50);
  }
  return false;
}
