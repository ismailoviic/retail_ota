#include "../include/reading_upload.h"

#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "../include/app_state.h"
#include "../include/mqtt_publish.h"
#include "../include/range_sensor.h"
#include "../include/wake_schedule.h"
#include "../include/wifi_link.h"

static char readingId[37] = {};
// Static, not on the setup() stack.
static char payload[PAYLOAD_CAPACITY];

static void measurementTimestamp(char* out, size_t outSize) {
  // The system clock survives deep sleep; sync after power-on and every 2 h for RC drift.
  // An invalid clock produces null, never a made-up server/measurement timestamp.
  uint64_t elapsed = elapsedNowMs();
  bool clockValid = retained.timeSynced && time(nullptr) >= VALID_TIME_AFTER;
  bool syncDue = !clockValid || elapsed - retained.lastNtpElapsedMs >= NTP_RESYNC_INTERVAL_MS;
  if (syncDue && networkBudgetAvailable(15000)) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    uint32_t start = millis();
    bool synced = false;
    while (true) {
      synced = sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
      if (synced || millis() - start >= NTP_WAIT_MS || !networkBudgetAvailable(15000)) break;
      delay(20);
    }
    if (synced) {
      retained.timeSynced = 1;
      retained.lastNtpElapsedMs = elapsed;
      clockValid = true;
    } else {
      LOGLN(clockValid ? "NTP resync failed; using the running clock." : "NTP sync failed.");
    }
  }
  time_t now = time(nullptr);
  if (!clockValid || now < VALID_TIME_AFTER) {
    strlcpy(out, "null", outSize);
    return;
  }
  time_t measured = now - (time_t)((millis() - sampleTakenMs) / 1000UL);
  struct tm utc;
  gmtime_r(&measured, &utc);
  strftime(out, outSize, "\"%Y-%m-%dT%H:%M:%SZ\"", &utc);
}

static const char* optionalInt(char* buffer, size_t size, bool present, long value) {
  if (!present) return "null";
  snprintf(buffer, size, "%ld", value);
  return buffer;
}

bool publishReading(float distance, float battery, bool plugged) {
  // Freeze BOTH UUID and payload before retrying. Retry diagnostics appear next wake.
  makeUuid(readingId);
  char measuredAt[32];
  measurementTimestamp(measuredAt, sizeof(measuredAt));

  bool sensorValid = distance != DISTANCE_UNAVAILABLE_CM;
  char fill[16] = "null";
  if (fillPercent >= 0) snprintf(fill, sizeof(fill), "%.2f", fillPercent);
  char error[48] = "null";
  if (!sensorValid) snprintf(error, sizeof(error), "\"%s\"", sensorError);
  char configVersion[12], spread[12], previousPublish[12], previousAwake[12];

  int length = snprintf(
    payload, sizeof(payload),
    "{\"device\":{\"device_id\":\"%s\",\"firmware_version\":%d,\"session_id\":\"%s\",\"config_version\":%s}"
    ",\"reading\":{\"reading_id\":\"%s\",\"sequence_no\":%lu,\"measured_at\":%s}"
    ",\"level\":{\"distance\":%.2f,\"fill_percent\":%s}"
    ",\"sensor\":{\"sensor_valid\":%s,\"valid_samples\":%d,\"sample_spread_mm\":%s,\"sensor_error\":%s}"
    ",\"power\":{\"battery_voltage\":%.3f,\"is_plugged\":%s}"
    ",\"schedule\":{\"interval_seconds\":%lu,\"schedule_reason\":\"%s\"}"
    ",\"network\":{\"wifi_rssi\":%d,\"wifi_connect_ms\":%lu}"
    ",\"diagnostics\":{\"wake_count\":%lu,\"wifi_failures_total\":%lu,\"upload_failures_total\":%lu"
    ",\"reset_reason\":%d,\"wakeup_cause\":%d,\"previous_publish_result\":%s,\"previous_awake_ms\":%s}}",
    // device
    deviceId, CURRENT_VERSION, retained.session,
    optionalInt(configVersion, sizeof(configVersion), calibrationValid(), CONFIG_VERSION),
    // reading
    readingId, (unsigned long)retained.sequence, measuredAt,
    // level
    distance, fill,
    // sensor
    sensorValid ? "true" : "false", sampleCount,
    optionalInt(spread, sizeof(spread), sampleSpread >= 0, sampleSpread),
    error,
    // power
    battery, plugged ? "true" : "false",
    // schedule
    (unsigned long)selectedInterval, scheduleReason,
    // network
    (int)WiFi.RSSI(), (unsigned long)wifiConnectMs,
    // diagnostics
    (unsigned long)retained.wakes, (unsigned long)retained.wifiFailures, (unsigned long)retained.uploadFailures,
    (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause(),
    optionalInt(previousPublish, sizeof(previousPublish), retained.previousPublishResult != 0, retained.previousPublishResult),
    optionalInt(previousAwake, sizeof(previousAwake), retained.previousAwakeMs != 0, (long)retained.previousAwakeMs));

  retained.previousPublishResult = 0;
  if (length < 0 || length >= (int)sizeof(payload)) {
    LOGLN("Payload exceeds buffer; publish skipped.");
    ++retained.uploadFailures;
    return false;
  }

  LOGF("Reading %s; session %s; sequence %lu\n", readingId,
       retained.session, (unsigned long)retained.sequence);
  return mqttPublish(payload, length);
}
