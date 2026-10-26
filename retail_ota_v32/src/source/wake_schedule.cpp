#include "../include/wake_schedule.h"

#include "../include/app_state.h"

float fillPercent = -1;
const char* scheduleReason = "uncalibrated";

bool calibrationValid() {
  return CALIBRATION_CONFIRMED && isfinite(DISTANCE_FULL_CM)
         && isfinite(DISTANCE_EMPTY_CM) && DISTANCE_FULL_CM >= 0
         && DISTANCE_EMPTY_CM > DISTANCE_FULL_CM;
}

// age 0 = newest reading in the fill history.
static int fillHistoryIndex(int age) {
  return (retained.fillHead - 1 - age + FILL_HISTORY) % FILL_HISTORY;
}

static void rememberFill(uint32_t minute, float fill) {
  retained.fillMinute[retained.fillHead] = minute;
  retained.fillTenths[retained.fillHead] = (int16_t)lroundf(fill * 10.0f);
  retained.fillHead = (retained.fillHead + 1) % FILL_HISTORY;
  if (retained.fillCount < FILL_HISTORY) ++retained.fillCount;
}

// Compares with the newest earlier reading that is at least 30 min old.
static bool levelStable(uint32_t minute, float fill) {
  for (int age = 0; age < retained.fillCount; ++age) {
    int i = fillHistoryIndex(age);
    uint32_t span = minute - retained.fillMinute[i];
    if (span < STABLE_MIN_SPAN_MINUTES) continue;
    float ratePerHour = fabsf(fill - retained.fillTenths[i] / 10.0f) * 60.0f / span;
    return ratePerHour < STABLE_RATE_PERCENT_PER_HOUR;
  }
  return false;
}

static bool overnight() {
  // No NTP since power-on: unknown hour, so day rules apply and no crossing is missed.
  time_t now = time(nullptr);
  if (!retained.timeSynced || now < VALID_TIME_AFTER) return false;
  struct tm utc;
  gmtime_r(&now, &utc);
  return utc.tm_hour >= NIGHT_START_HOUR_UTC || utc.tm_hour < NIGHT_END_HOUR_UTC;
}

uint32_t chooseInterval(float distance) {
  fillPercent = -1;
  if (!calibrationValid()) {
    scheduleReason = "uncalibrated";
    return NORMAL_INTERVAL;
  }
  if (distance == DISTANCE_UNAVAILABLE_CM) {
    ++retained.sensorFailWakes;
    scheduleReason = "sensor_retry";
    return retained.sensorFailWakes < SENSOR_RETRY_WAKES ? DRAINING_INTERVAL : HIGH_LEVEL_INTERVAL;
  }
  retained.sensorFailWakes = 0;
  fillPercent = constrain(100.0f * (DISTANCE_EMPTY_CM - distance)
                            / (DISTANCE_EMPTY_CM - DISTANCE_FULL_CM),
                          0.0f, 100.0f);

  uint32_t minute = (uint32_t)((retained.elapsedMs + (sampleTakenMs - cycleStartMs)) / 60000ULL);
  bool refilled = retained.fillCount > 0
                  && fillPercent - retained.fillTenths[fillHistoryIndex(0)] / 10.0f > REFILL_JUMP_PERCENT;
  uint32_t interval;
  if (refilled) {
    interval = REFILL_HOLD_INTERVAL;
    scheduleReason = "refill_hold";
  } else if (overnight() && levelStable(minute, fillPercent)) {
    interval = NIGHT_STABLE_INTERVAL;
    scheduleReason = "night_stable";
  } else if (fillPercent >= SCHEDULE_HIGH_LEVEL_PERCENT) {
    interval = HIGH_LEVEL_INTERVAL;
    scheduleReason = "high_level";
  } else {
    interval = DRAINING_INTERVAL;
    scheduleReason = "draining";
  }
  rememberFill(minute, fillPercent);
  return interval;
}
