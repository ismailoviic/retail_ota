#include "../include/reading_upload.h"

#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "../include/app_state.h"
#include "../include/awake_watchdog.h"
#include "../include/mqtt_publish.h"
#include "../include/range_sensor.h"
#include "../include/track_store.h"
#include "../include/wake_schedule.h"
#include "../include/wifi_link.h"

static char readingId[37] = {};
static char trackId[37] = {};
// Static, not on the setup() stack; the reading and then the track are built in it.
static char payload[PAYLOAD_CAPACITY];

static const char* optionalInt(char* buffer, size_t size, bool present, long value) {
  if (!present) return "null";
  snprintf(buffer, size, "%ld", value);
  return buffer;
}

bool publishReading(float distance, float battery, bool plugged) {
  // Freeze BOTH UUID and payload before retrying. Retry diagnostics appear next wake.
  makeUuid(readingId);

  bool sensorValid = distance != DISTANCE_UNAVAILABLE_CM;
  char fill[16] = "null";
  if (fillPercent >= 0) snprintf(fill, sizeof(fill), "%.2f", fillPercent);
  char error[48] = "null";
  if (!sensorValid) snprintf(error, sizeof(error), "\"%s\"", sensorError);
  char configVersion[12], spread[12], previousPublish[12], previousAwake[12], confirmReadings[12];
  char hangStage[16] = "null";
  const char* stageName = wakeStageName(retained.pendingHangStage);
  if (stageName) snprintf(hangStage, sizeof(hangStage), "\"%s\"", stageName);

  int length = snprintf(
    payload, sizeof(payload),
    "{\"device\":{\"device_id\":\"%s\",\"firmware_version\":%d,\"session_id\":\"%s\",\"config_version\":%s}"
    ",\"reading\":{\"reading_id\":\"%s\",\"sequence_no\":%lu}"
    ",\"level\":{\"distance\":%.2f,\"fill_percent\":%s}"
    ",\"sensor\":{\"sensor_valid\":%s,\"valid_samples\":%d,\"sample_spread_mm\":%s,\"sensor_error\":%s}"
    ",\"power\":{\"battery_voltage\":%.3f,\"is_plugged\":%s}"
    ",\"schedule\":{\"interval_seconds\":%lu,\"schedule_reason\":\"%s\",\"refill_confirm_readings\":%s}"
    ",\"network\":{\"wifi_rssi\":%d,\"wifi_connect_ms\":%lu}"
    ",\"diagnostics\":{\"wake_count\":%lu,\"wifi_failures_total\":%lu,\"upload_failures_total\":%lu"
    ",\"reset_reason\":%d,\"wakeup_cause\":%d,\"previous_publish_result\":%s,\"previous_awake_ms\":%s"
    ",\"watchdog_trips\":%lu,\"hang_stage\":%s,\"state_restored\":%s,\"tracks_dropped_total\":%lu}}",
    // device
    deviceId, CURRENT_VERSION, retained.session,
    optionalInt(configVersion, sizeof(configVersion), calibrationValid(), CONFIG_VERSION),
    // reading
    readingId, (unsigned long)retained.sequence,
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
    optionalInt(confirmReadings, sizeof(confirmReadings), refillConfirmReadings > 0, refillConfirmReadings),
    // network
    (int)WiFi.RSSI(), (unsigned long)wifiConnectMs,
    // diagnostics
    (unsigned long)retained.wakes, (unsigned long)retained.wifiFailures, (unsigned long)retained.uploadFailures,
    (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause(),
    optionalInt(previousPublish, sizeof(previousPublish), retained.previousPublishResult != 0, retained.previousPublishResult),
    optionalInt(previousAwake, sizeof(previousAwake), retained.previousAwakeMs != 0, (long)retained.previousAwakeMs),
    (unsigned long)retained.watchdogTrips, hangStage, retained.stateRestored ? "true" : "false",
    (unsigned long)retained.tracksDropped);

  retained.previousPublishResult = 0;
  if (length < 0 || length >= (int)sizeof(payload)) {
    LOGLN("Payload exceeds buffer; publish skipped.");
    ++retained.uploadFailures;
    return false;
  }

  LOGF("Reading %s; session %s; sequence %lu\n", readingId,
       retained.session, (unsigned long)retained.sequence);
  bool sent = mqttPublish(payload, length);
  if (sent) {
    retained.pendingHangStage = STAGE_NONE;  // Each trip and restore is reported once
    retained.stateRestored = 0;
  }
  return sent;
}

bool publishTrack() {
  // {"device":{...},"track":{"track_id","first_sequence_no","count","interval_seconds",
  //  "last_sample_age_ms","fill_percent":[oldest ... newest, null = sensor failure]}}
  makeUuid(trackId);
  int count = trackCount();
  int length = snprintf(
    payload, sizeof(payload),
    "{\"device\":{\"device_id\":\"%s\",\"firmware_version\":%d,\"session_id\":\"%s\"}"
    ",\"track\":{\"track_id\":\"%s\",\"first_sequence_no\":%lu,\"count\":%d,\"interval_seconds\":%lu"
    ",\"last_sample_age_ms\":%lu,\"fill_percent\":[",
    deviceId, CURRENT_VERSION, retained.session, trackId, (unsigned long)trackFirstSequence(), count,
    (unsigned long)OFFLINE_INTERVAL, (unsigned long)trackLastSampleAgeMs());
  for (int i = 0; i < count && length > 0 && length < (int)sizeof(payload); ++i) {
    int16_t tenths = trackSampleTenths(i);
    const char* separator = i ? "," : "";
    length += tenths < 0
                ? snprintf(payload + length, sizeof(payload) - length, "%snull", separator)
                : snprintf(payload + length, sizeof(payload) - length, "%s%d.%d", separator,
                           tenths / 10, tenths % 10);
  }
  if (length > 0 && length < (int)sizeof(payload)) {
    length += snprintf(payload + length, sizeof(payload) - length, "]}}");
  }
  if (length <= 0 || length >= (int)sizeof(payload)) {
    LOGLN("Track exceeds buffer; kept for the next upload.");
    return false;
  }

  LOGF("Track %s; %d samples\n", trackId, count);
  if (!mqttPublish(payload, length)) return false;
  clearTrack();
  return true;
}
