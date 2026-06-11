#pragma once
#include <Arduino.h>
#include "config.h"

void portalBegin(Config *cfg);
void portalEnd();
bool portalSaveRequested();
