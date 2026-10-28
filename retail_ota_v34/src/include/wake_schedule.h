#pragma once

// Refill-aware wake schedule (modes in app_state.h):
//   online          30 min, published; <= 15 % -> offline; jump > 15 pts -> refill_confirm
//   offline         1 min, radio off, into the track; jump -> refill_confirm; track full -> upload
//   refill_confirm  1 min, radio off; 5 consecutive readings within 1 pt -> publish + hold;
//                   10 readings without, or back near the pre-jump level -> previous mode
//   hold            4 h, or until 06:00 UTC after an evening refill (NTP on that wake only)

#include <stdint.h>

extern float fillPercent;           // -1 = unknown
extern const char* scheduleReason;  // Published with each reading
extern bool publishThisWake;        // Wi-Fi and MQTT needed this wake
extern bool nightCheckDue;          // Call applyNightHold() once Wi-Fi is connected
extern int refillConfirmReadings;   // Readings the confirmed refill took; -1 = none this wake

bool calibrationValid();
// Computes fillPercent from distance (cm), advances the mode, records the fill and returns
// the seconds until the next wake. Call after sampleTakenMs is set.
uint32_t chooseInterval(float distance);
// After a confirmed refill (or a retry): NTP, then night_hold until 06:00 UTC if it is
// 17:00-05:59 UTC and the level is >= 50 %. Returns the interval to use.
uint32_t applyNightHold(uint32_t interval);
