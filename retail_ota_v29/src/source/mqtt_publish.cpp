#include "../include/mqtt_publish.h"

#include <WiFi.h>
#include <MQTT.h>
#include <esp_sntp.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "../include/app_state.h"
#include "../include/range_sensor.h"
#include "../include/wake_schedule.h"
#include "../include/wifi_link.h"

static char readingId[37] = {};

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

static const char* optionalFloat(char* buffer, size_t size, bool present, float value, int decimals) {
  if (!present) return "null";
  snprintf(buffer, size, "%.*f", decimals, value);
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
  char sigma[12], signal[12], ambient[12];

  // Grouped by topic; field names match the v28 columns, so a subscriber can flatten the
  // groups straight into the existing table.
  char payload[PAYLOAD_CAPACITY];
  int length = snprintf(
    payload, sizeof(payload),
    "{\"device\":{\"device_id\":\"%s\",\"firmware_version\":%d,\"session_id\":\"%s\",\"config_version\":%s}"
    ",\"reading\":{\"reading_id\":\"%s\",\"sequence_no\":%lu,\"measured_at\":%s}"
    ",\"level\":{\"distance\":%.2f,\"fill_percent\":%s}"
    ",\"sensor\":{\"sensor_valid\":%s,\"valid_samples\":%d,\"sample_spread_mm\":%s,\"sensor_error\":%s"
    ",\"range_sigma_mm\":%s,\"signal_rate_mcps\":%s,\"ambient_rate_mcps\":%s,\"range_budget_ms\":%lu}"
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
    optionalFloat(sigma, sizeof(sigma), rangeSigmaMm >= 0, rangeSigmaMm, 1),
    optionalFloat(signal, sizeof(signal), rangeSignalMcps >= 0, rangeSignalMcps, 3),
    optionalFloat(ambient, sizeof(ambient), rangeAmbientMcps >= 0, rangeAmbientMcps, 3),
    (unsigned long)rangeBudgetMs,
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

  char topic[96];  // storeyes/<36-char id>/coffee-counter = 60 characters
  snprintf(topic, sizeof(topic), "%s%s%s", MQTT_TOPIC_PREFIX, deviceId, MQTT_TOPIC_SUFFIX);

  LOGF("Reading %s; session %s; sequence %lu; topic %s\n", readingId,
       retained.session, (unsigned long)retained.sequence, topic);
  for (int attempt = 1; attempt <= 3; ++attempt) {
    // Stage limits are not a guaranteed total request deadline.
    if (WiFi.status() != WL_CONNECTED || !networkBudgetAvailable(12000)) break;
    WiFiClient net;  // Plain TCP: the payload and any username/password travel unencrypted.
    MQTTClient mqtt(256, MQTT_BUFFER_SIZE);  // Reads only CONNACK/PUBACK.
    mqtt.begin(MQTT_HOST, MQTT_PORT, net);
    mqtt.setOptions(MQTT_KEEPALIVE_SECONDS, true, MQTT_TIMEOUT_MS);
    // Clean session, no will, no subscriptions: connect, publish, wait for PUBACK, disconnect.
    bool acknowledged = mqtt.connect(deviceId, MQTT_USERNAME, MQTT_PASSWORD)
                        && mqtt.publish(topic, payload, length, false, MQTT_QOS);
    lwmqtt_err_t error = mqtt.lastError();
    lwmqtt_return_code_t returnCode = mqtt.returnCode();
    retained.previousPublishResult = acknowledged ? 1 : (error != LWMQTT_SUCCESS ? (int32_t)error : -1);
    if (mqtt.connected()) mqtt.disconnect();
    else net.stop();
    LOGF("MQTT attempt %d/3: %s; error %d; CONNACK code %d\n", attempt,
         acknowledged ? "acknowledged" : "failed", (int)error, (int)returnCode);
    if (acknowledged) return true;
    ++retained.uploadFailures;
    // No repeated credential, authorization, client-ID or size errors.
    bool permanent = error == LWMQTT_BUFFER_TOO_SHORT
                     || (error == LWMQTT_CONNECTION_DENIED && returnCode != LWMQTT_SERVER_UNAVAILABLE);
    if (permanent) break;
    if (attempt < 3 && networkBudgetAvailable(13000)) delay(500 * attempt);
  }
  LOGLN("Reading not acknowledged; no offline queue. Next sequence will advance.");
  return false;
}
