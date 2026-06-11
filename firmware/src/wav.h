#pragma once
#include <Arduino.h>
#include <FS.h>

void wavWritePlaceholderHeader(File &f, uint32_t sample_rate, uint16_t channels, uint16_t bits);

// Patch the RIFF and data size fields of a WAV file by path. The file MUST be
// closed first — this opens it in "r+" mode (read+write, no truncate), seeks +
// writes the two size fields, and closes again. Each open/close cycle forces a
// FatFs flush, which makes the patch survive even when the SD bus is iffy.
// Retries internally on transient open/seek failures. Returns true on success.
bool wavPatchSizes(const char *path, uint32_t total_file_bytes);
