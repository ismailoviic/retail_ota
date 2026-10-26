#pragma once

// Refill-aware wake schedule: 4 h after a refill, 1 h while >= 30 %, 5 min below,
// 1 h overnight when the level is stable.

#include <stdint.h>

extern float fillPercent;           // -1 = unknown
extern const char* scheduleReason;  // Published with each reading

bool calibrationValid();
// Computes fillPercent from distance (cm), records it in the RTC history and returns
// the seconds until the next wake. Call after sampleTakenMs is set.
uint32_t chooseInterval(float distance);
