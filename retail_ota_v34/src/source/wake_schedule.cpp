#include "../include/wake_schedule.h"

#include <esp_sntp.h>

#include "../include/app_state.h"
#include "../include/track_store.h"

float fillPercent = -1;
const char* scheduleReason = "uncalibrated";
bool publishThisWake = true;
bool nightCheckDue = false;
int refillConfirmReadings = -1;

bool calibrationValid() {
  return CALIBRATION_CONFIRMED && isfinite(DISTANCE_FULL_CM)
         && isfinite(DISTANCE_EMPTY_CM) && DISTANCE_FULL_CM >= 0
         && DISTANCE_EMPTY_CM > DISTANCE_FULL_CM;
}

// age 0 = newest reading in the fill history.
static int fillHistoryIndex(int age) {
  return (retained.fillHead - 1 - age + FILL_HISTORY) % FILL_HISTORY;
}

static void rememberFill(int16_t tenths) {
  retained.fillTenths[retained.fillHead] = tenths;
  retained.fillHead = (retained.fillHead + 1) % FILL_HISTORY;
  if (retained.fillCount < FILL_HISTORY) ++retained.fillCount;
}

// Jump reference: the lowest recent fill, so a refill poured over a few 1 min readings counts.
static int16_t lowestRecentTenths() {
  int16_t lowest = INT16_MAX;
  for (int age = 0; age < retained.fillCount; ++age) {
    lowest = min(lowest, retained.fillTenths[fillHistoryIndex(age)]);
  }
  return lowest;
}

// Spread of the newest REFILL_CONFIRM_READINGS fills, in tenths.
static int recentSpreadTenths() {
  int16_t lowest = INT16_MAX, highest = INT16_MIN;
  for (int age = 0; age < REFILL_CONFIRM_READINGS; ++age) {
    int16_t value = retained.fillTenths[fillHistoryIndex(age)];
    lowest = min(lowest, value);
    highest = max(highest, value);
  }
  return highest - lowest;
}

static void enterRefillConfirm(int16_t preJumpTenths) {
  retained.modeBeforeConfirm = retained.mode;
  retained.mode = MODE_REFILL_CONFIRM;
  retained.preJumpTenths = preJumpTenths;
  retained.confirmReadings = 1;  // The jump reading itself
  retained.confirmValid = 1;
  scheduleReason = "refill_confirm";
  publishThisWake = false;
}

// Jump not confirmed (a hand or scoop, or still pouring): continue by level, as if no refill.
static uint32_t rejectRefill(float fill) {
  bool wasOnline = retained.modeBeforeConfirm == MODE_ONLINE;
  if (fill > OFFLINE_THRESHOLD_PERCENT) {
    retained.mode = MODE_ONLINE;
    scheduleReason = "refill_rejected";  // Published, so false refills are visible
    publishThisWake = true;
    return ONLINE_INTERVAL;
  }
  retained.mode = MODE_OFFLINE;
  scheduleReason = wasOnline ? "going_offline" : "offline";
  publishThisWake = wasOnline || trackUploadDue();
  return OFFLINE_INTERVAL;
}

uint32_t chooseInterval(float distance) {
  fillPercent = -1;
  publishThisWake = true;
  nightCheckDue = false;
  refillConfirmReadings = -1;
  if (!calibrationValid()) {
    scheduleReason = "uncalibrated";
    return NORMAL_INTERVAL;
  }
  bool radioOff = radioOffMode();
  if (distance == DISTANCE_UNAVAILABLE_CM) {
    ++retained.sensorFailWakes;
    scheduleReason = "sensor_retry";
    if (!radioOff) {
      return retained.sensorFailWakes < SENSOR_RETRY_WAKES ? SENSOR_RETRY_INTERVAL : ONLINE_INTERVAL;
    }
    appendTrackSample(-1);
    if (retained.mode == MODE_REFILL_CONFIRM) {
      retained.confirmValid = 0;  // Confirmation needs consecutive valid readings
      if (++retained.confirmReadings >= REFILL_CONFIRM_MAX_READINGS) {
        return rejectRefill(retained.fillTenths[fillHistoryIndex(0)] / 10.0f);
      }
      publishThisWake = false;
    } else {
      publishThisWake = trackUploadDue();
    }
    return OFFLINE_INTERVAL;
  }
  retained.sensorFailWakes = 0;
  fillPercent = constrain(100.0f * (DISTANCE_EMPTY_CM - distance)
                            / (DISTANCE_EMPTY_CM - DISTANCE_FULL_CM),
                          0.0f, 100.0f);
  int16_t tenths = (int16_t)lroundf(fillPercent * 10.0f);
  int16_t lowest = lowestRecentTenths();
  bool jump = retained.fillCount > 0 && tenths - lowest > lroundf(REFILL_JUMP_PERCENT * 10.0f);
  rememberFill(tenths);
  if (radioOff) appendTrackSample(fillPercent);

  if (retained.mode == MODE_REFILL_CONFIRM) {
    ++retained.confirmReadings;
    ++retained.confirmValid;
    if (tenths - retained.preJumpTenths <= lroundf(REFILL_JUMP_PERCENT * 10.0f)) {
      return rejectRefill(fillPercent);  // Back near the pre-jump level
    }
    if (retained.confirmValid >= REFILL_CONFIRM_READINGS
        && recentSpreadTenths() <= lroundf(REFILL_CONFIRM_BAND_PERCENT * 10.0f)) {
      refillConfirmReadings = retained.confirmReadings;
      retained.mode = MODE_ONLINE;
      retained.retryNightCheck = 1;  // Cleared once NTP answers
      nightCheckDue = true;
      scheduleReason = "refill_hold";
      return REFILL_HOLD_INTERVAL;
    }
    if (retained.confirmReadings >= REFILL_CONFIRM_MAX_READINGS) return rejectRefill(fillPercent);
    scheduleReason = "refill_confirm";
    publishThisWake = false;
    return OFFLINE_INTERVAL;
  }

  if (retained.mode == MODE_OFFLINE) {
    if (jump) {
      enterRefillConfirm(lowest);
      return OFFLINE_INTERVAL;
    }
    publishThisWake = trackUploadDue();
    scheduleReason = publishThisWake ? "track_full" : "offline";
    return OFFLINE_INTERVAL;
  }

  // MODE_ONLINE
  if (jump) {
    appendTrackSample(fillPercent);  // First reading of the refill check, radio off from now
    enterRefillConfirm(lowest);
    return OFFLINE_INTERVAL;
  }
  if (fillPercent <= OFFLINE_THRESHOLD_PERCENT) {
    retained.mode = MODE_OFFLINE;
    scheduleReason = "going_offline";  // Published: the last reading before the radio goes off
    return OFFLINE_INTERVAL;
  }
  scheduleReason = "online";
  nightCheckDue = retained.retryNightCheck;
  return ONLINE_INTERVAL;
}

uint32_t applyNightHold(uint32_t interval) {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  uint32_t start = millis();
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
    if (millis() - start >= NTP_WAIT_MS || !networkBudgetAvailable(15000)) break;
    delay(20);
  }
  time_t now = time(nullptr);
  if (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED || now < VALID_TIME_AFTER) {
    LOGLN("NTP failed; normal interval, retried at the next online wake.");
    return interval;
  }
  retained.retryNightCheck = 0;
  struct tm utc;
  gmtime_r(&now, &utc);
  bool evening = utc.tm_hour >= NIGHT_HOLD_START_HOUR_UTC || utc.tm_hour < NIGHT_HOLD_END_HOUR_UTC;
  LOGF("UTC %02d:%02d; fill %.1f %%\n", utc.tm_hour, utc.tm_min, fillPercent);
  if (!evening || fillPercent < NIGHT_HOLD_MIN_PERCENT) return interval;

  int32_t nowSeconds = utc.tm_hour * 3600 + utc.tm_min * 60 + utc.tm_sec;
  int32_t endSeconds = NIGHT_HOLD_END_HOUR_UTC * 3600;
  uint32_t untilEnd = nowSeconds < endSeconds ? endSeconds - nowSeconds : 24 * 3600 - nowSeconds + endSeconds;
  scheduleReason = "night_hold";
  // goToDeepSleep counts the interval from the start of this wake.
  return untilEnd + (millis() - cycleStartMs) / 1000UL;
}
