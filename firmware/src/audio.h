#pragma once
#include <Arduino.h>

bool   audioBegin();
void   audioEnd();
void   audioDropSettle();
size_t audioReadFrames(int16_t *out, size_t maxOutInt16);
