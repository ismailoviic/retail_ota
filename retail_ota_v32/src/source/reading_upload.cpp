#include "../include/reading_upload.h"

#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "../include/app_state.h"
#include "../include/mqtt_publish.h"
#include "../include/range_sensor.h"
#include "../include/supabase_upload.h"
#include "../include/wake_schedule.h"
#include "../include/wifi_link.h"

static char readingId[37] = {};
// Static, not on the setup() stack, which also holds the TLS client.
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

// Values shared by both payloads, frozen once per wake.
struct ReadingFields {
  const char* measuredAt;
  const char* sensorValid;
  const char* fill;
  const char* error;
  const char* configVersion;
  const char* spread;
  const char* previousPublish;
  const char* previousHttp;
  const char* previousAwake;
};

// Flat v28 row: exactly the existing sensor_data columns, or PostgREST rejects the insert.
static int buildSupabasePayload(const ReadingFields& f, float distance, float battery, bool plugged) {
  return snprintf(
    payload, sizeof(payload),
    "{\"distance\":%.2f,\"battery_voltage\":%.3f,\"is_plugged\":%s,\"firmware_version\":%d"
    ",\"device_id\":\"%s\",\"reading_id\":\"%s\",\"session_id\":\"%s\""
    ",\"sequence_no\":%lu,\"wake_count\":%lu,\"wifi_failures_total\":%lu,\"upload_failures_total\":%lu"
    ",\"measured_at\":%s,\"fill_percent\":%s,\"interval_seconds\":%lu,\"config_version\":%s"
    ",\"sensor_valid\":%s,\"valid_samples\":%d,\"sample_spread_mm\":%s,\"sensor_error\":%s"
    ",\"wifi_rssi\":%d,\"wifi_connect_ms\":%lu,\"reset_reason\":%d,\"wakeup_cause\":%d"
    ",\"previous_upload_http_status\":%s,\"previous_awake_ms\":%s}",
    distance, battery, plugged ? "true" : "false", CURRENT_VERSION,
    deviceId, readingId, retained.session,
    (unsigned long)retained.sequence, (unsigned long)retained.wakes,
    (unsigned long)retained.wifiFailures, (unsigned long)retained.uploadFailures,
    f.measuredAt, f.fill, (unsigned long)selectedInterval, f.configVersion,
    f.sensorValid, sampleCount, f.spread, f.error,
    (int)WiFi.RSSI(), (unsigned long)wifiConnectMs, (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause(),
    f.previousHttp, f.previousAwake);
}

// Grouped by topic; field names inside the groups are the v28 column names.
static int buildMqttPayload(const ReadingFields& f, float distance, float battery, bool plugged) {
  return snprintf(
    payload, sizeof(payload),
    "{\"device\":{\"device_id\":\"%s\",\"firmware_version\":%d,\"session_id\":\"%s\",\"config_version\":%s}"
    ",\"reading\":{\"reading_id\":\"%s\",\"sequence_no\":%lu,\"measured_at\":%s}"
    ",\"level\":{\"distance\":%.2f,\"fill_percent\":%s}"
    ",\"sensor\":{\"sensor_valid\":%s,\"valid_samples\":%d,\"sample_spread_mm\":%s,\"sensor_error\":%s}"
    ",\"power\":{\"battery_voltage\":%.3f,\"is_plugged\":%s}"
    ",\"schedule\":{\"interval_seconds\":%lu,\"schedule_reason\":\"%s\"}"
    ",\"network\":{\"wifi_rssi\":%d,\"wifi_connect_ms\":%lu}"
    ",\"diagnostics\":{\"wake_count\":%lu,\"wifi_failures_total\":%lu,\"upload_failures_total\":%lu"
    ",\"reset_reason\":%d,\"wakeup_cause\":%d,\"previous_publish_result\":%s"
    ",\"previous_upload_http_status\":%s,\"previous_awake_ms\":%s}}",
    // device
    deviceId, CURRENT_VERSION, retained.session, f.configVersion,
    // reading
    readingId, (unsigned long)retained.sequence, f.measuredAt,
    // level
    distance, f.fill,
    // sensor
    f.sensorValid, sampleCount, f.spread, f.error,
    // power
    battery, plugged ? "true" : "false",
    // schedule
    (unsigned long)selectedInterval, scheduleReason,
    // network
    (int)WiFi.RSSI(), (unsigned long)wifiConnectMs,
    // diagnostics
    (unsigned long)retained.wakes, (unsigned long)retained.wifiFailures, (unsigned long)retained.uploadFailures,
    (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause(),
    f.previousPublish, f.previousHttp, f.previousAwake);
}

static bool payloadFits(int length, const char* channel) {
  if (length >= 0 && length < (int)sizeof(payload)) return true;
  LOGF("%s payload exceeds buffer; skipped.\n", channel);
  ++retained.uploadFailures;
  return false;
}

bool publishReading(float distance, float battery, bool plugged) {
  // Freeze the UUID and the previous-wake results before either channel changes them.
  makeUuid(readingId);
  char measuredAt[32];
  measurementTimestamp(measuredAt, sizeof(measuredAt));

  bool sensorValid = distance != DISTANCE_UNAVAILABLE_CM;
  char fill[16] = "null";
  if (fillPercent >= 0) snprintf(fill, sizeof(fill), "%.2f", fillPercent);
  char error[48] = "null";
  if (!sensorValid) snprintf(error, sizeof(error), "\"%s\"", sensorError);
  char configVersion[12], spread[12], previousPublish[12], previousHttp[12], previousAwake[12];

  ReadingFields fields = {
    measuredAt,
    sensorValid ? "true" : "false",
    fill,
    error,
    optionalInt(configVersion, sizeof(configVersion), calibrationValid(), CONFIG_VERSION),
    optionalInt(spread, sizeof(spread), sampleSpread >= 0, sampleSpread),
    optionalInt(previousPublish, sizeof(previousPublish), retained.previousPublishResult != 0, retained.previousPublishResult),
    optionalInt(previousHttp, sizeof(previousHttp), retained.previousHttp != 0, retained.previousHttp),
    optionalInt(previousAwake, sizeof(previousAwake), retained.previousAwakeMs != 0, (long)retained.previousAwakeMs),
  };
  retained.previousPublishResult = 0;
  retained.previousHttp = 0;

  LOGF("Reading %s; session %s; sequence %lu\n", readingId,
       retained.session, (unsigned long)retained.sequence);

  // Supabase first: it feeds the dashboard. A failure there does not skip MQTT.
  int length = buildSupabasePayload(fields, distance, battery, plugged);
  bool stored = payloadFits(length, "Supabase") && supabaseUpload(payload, length);

  length = buildMqttPayload(fields, distance, battery, plugged);
  bool published = payloadFits(length, "MQTT") && mqttPublish(payload, length);

  return stored && published;
}
