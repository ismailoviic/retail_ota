#pragma once

// All tunable constants for firmware 34. constexpr keeps each one private to the file
// that includes it, so this header can be shared without duplicate definitions.

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#if !defined(CONFIG_IDF_TARGET_ESP32C3)
#error "Select an ESP32-C3 board."
#endif

// --------------------------------------------------
// Logging
// --------------------------------------------------

// 1 for USB testing. 0 for release: no serial formatting or flush on each wake.
#ifndef ENABLE_SERIAL_LOG
#define ENABLE_SERIAL_LOG 0
#endif

#if ENABLE_SERIAL_LOG
#define LOGF(...) Serial.printf(__VA_ARGS__)
#define LOGLN(...) Serial.println(__VA_ARGS__)
#else
#define LOGF(...) \
  do { \
  } while (0)
#define LOGLN(...) \
  do { \
  } while (0)
#endif

// --------------------------------------------------
// Firmware and timing
// --------------------------------------------------

constexpr int CURRENT_VERSION = 34;
// Used only on FIRST provisioning. Subsequent OTA firmware keeps the stored ID.
// Before first use on another ESP, register it and replace this initial UUID.
constexpr const char* INITIAL_DEVICE_ID = "4f974379-a444-4c50-8cca-175d7803ceda";  // "e2a880ac-c44f-4708-b315-9b6a3eab727a";  //=> the comment is the test device ID version 26

// Dashboard calibration: 26.7 cm = 11 %, 27.0 cm = 10 %, 8.7 cm = 71 % (25 Oct readings).
constexpr bool CALIBRATION_CONFIRMED = true;
constexpr float DISTANCE_FULL_CM = 0.0f;
constexpr float DISTANCE_EMPTY_CM = 30.0f;
constexpr int CONFIG_VERSION = 2;
constexpr uint32_t NORMAL_INTERVAL = 600;  // Only without calibration.

// Wake schedule (modes in wake_schedule.h): 30 min online down to 15 %, then 1 min readings
// with the radio off until a confirmed refill, then a 4 h hold (until 06:00 UTC in the evening).
constexpr uint32_t ONLINE_INTERVAL = 1800;
constexpr float OFFLINE_THRESHOLD_PERCENT = 15.0f;
constexpr uint32_t OFFLINE_INTERVAL = 60;           // Radio-off readings: offline and refill_confirm.
constexpr uint32_t REFILL_HOLD_INTERVAL = 4 * 3600;  // From ~70 % the level cannot reach 15 % in 4 h.
constexpr float REFILL_JUMP_PERCENT = 15.0f;
// A jump is a refill only once REFILL_CONFIRM_READINGS consecutive 1 min readings stay within
// the band: a hand or scoop under the sensor moves away. Gives up after MAX readings.
constexpr int REFILL_CONFIRM_READINGS = 5;
constexpr float REFILL_CONFIRM_BAND_PERCENT = 1.0f;
constexpr int REFILL_CONFIRM_MAX_READINGS = 10;
// Evening refill (17:00-05:59 UTC, level >= 50 %): sleep until 06:00 UTC. Refills on 26-27 Oct
// were at ~18:00 and ~17:54 UTC (70-73 %), midday ones ~11:30 UTC; drawing starts ~07:00 UTC.
constexpr int NIGHT_HOLD_START_HOUR_UTC = 17;
constexpr int NIGHT_HOLD_END_HOUR_UTC = 6;
constexpr float NIGHT_HOLD_MIN_PERCENT = 50.0f;
constexpr int FILL_HISTORY = REFILL_CONFIRM_READINGS;
constexpr uint32_t SENSOR_RETRY_WAKES = 3;  // Unreadable sensor while online: 5 min retries, then 30 min.
constexpr uint32_t SENSOR_RETRY_INTERVAL = 300;

// Offline track: one fill value per radio-off reading, uploaded after the next reading.
constexpr int TRACK_CAPACITY = 720;              // 12 h at 1 min; 1.4 KB of RTC memory
constexpr int TRACK_FULL_RETRY_READINGS = 60;    // Full and unsent: retry the upload hourly.

// NTP only on the wake that chooses the hold after a refill.
constexpr uint32_t NTP_WAIT_MS = 3000;
constexpr time_t VALID_TIME_AFTER = 1735689600;  // 2025-01-01

// Watchdog: a wake longer than its deadline goes straight to deep sleep; the RTC watchdog
// resets the chip if even that cannot run.
constexpr uint32_t RADIO_OFF_DEADLINE_MS = 20000;
constexpr uint32_t ONLINE_DEADLINE_MS = 60000;
constexpr uint32_t OTA_CHUNK_DEADLINE_MS = 20000;  // Extended on every downloaded chunk.
constexpr uint32_t RTC_WATCHDOG_MS = 600000;
constexpr uint32_t DEADLINE_SLEEP_RADIO_OFF = 60;
constexpr uint32_t DEADLINE_SLEEP_ONLINE = 300;

constexpr uint32_t MIN_SLEEP_MS = 5000;
constexpr uint32_t NETWORK_WORK_BUDGET_MS = 45000;
constexpr uint64_t OTA_CHECK_INTERVAL_MS = 24ULL * 3600ULL * 1000ULL;

// Timer wakes only; power-on/RESET always runs the full recovery path.
constexpr uint32_t WIFI_BACKOFF_AFTER_FAILED_WAKES = 3;
constexpr uint32_t WIFI_BACKOFF_INTERVAL = 3600;

constexpr unsigned long WIFI_CONNECT_TIMEOUT_SECONDS = 12;
// The cached AP must associate within this time or the full scan takes over.
constexpr uint32_t FAST_ASSOCIATE_TIMEOUT_MS = 2500;
constexpr unsigned long PORTAL_TIMEOUT_SECONDS = 180;
constexpr uint32_t PORTAL_DEADLINE_MS = (PORTAL_TIMEOUT_SECONDS + 60) * 1000UL;

// Set true only to deliberately open the configuration portal.
constexpr bool FORCE_CONFIG_PORTAL = false;

// --------------------------------------------------
// GPIO assignments
// --------------------------------------------------

constexpr int BATTERY_PIN = 0;
constexpr int PLUGIN_PIN = 1;
constexpr int SDA_PIN = 4;
constexpr int SCL_PIN = 5;

constexpr int LED_PIN = 8;
constexpr bool LED_ACTIVE_LOW = true;
// Status blinking is install feedback; unattended timer wakes keep the LED off.
constexpr bool LED_ON_TIMER_WAKE = false;

// Preserve v23 configuration. No XSHUT wiring change required.
constexpr int XSHUT_PIN = 6;
constexpr bool USE_XSHUT = false;

// --------------------------------------------------
// Distance measurement
// --------------------------------------------------

// v28 median filter (firmware 29's sigma grading reported no distance on the deployed board),
// with 10 samples instead of 5.
constexpr uint32_t RANGE_TIMING_BUDGET_US = 100000;
constexpr int RANGE_SAMPLE_COUNT = 10;
constexpr int RANGE_MIN_VALID_SAMPLES = 3;
constexpr uint32_t RANGE_SAMPLE_GAP_MS = 20;

constexpr uint16_t RANGE_MIN_MM = 20;
constexpr uint16_t RANGE_MAX_MM = 2000;
constexpr float DISTANCE_UNAVAILABLE_CM = 999.0f;

// 100 kHz as in v28, proven on this wiring.
constexpr uint32_t I2C_CLOCK_HZ = 100000;

// --------------------------------------------------
// Current voltage dividers
// --------------------------------------------------

constexpr float BATTERY_R_TOP = 10000.0f;
constexpr float BATTERY_R_BOTTOM = 10000.0f;

constexpr float USB_R_TOP = 10000.0f;
constexpr float USB_R_BOTTOM = 10000.0f;

constexpr float BATTERY_GAIN = 1.0f;
constexpr float BATTERY_OFFSET_V = 0.0f;
constexpr float USB_PRESENT_THRESHOLD_V = 4.0f;

// --------------------------------------------------
// Wi-Fi
// --------------------------------------------------

constexpr wifi_power_t WIFI_TX_POWER = WIFI_POWER_8_5dBm;

// --------------------------------------------------
// MQTT — the only upload; Supabase rejected firmware 32's rows (HTTP 400)
// --------------------------------------------------

constexpr const char* MQTT_HOST = "18.100.207.236";
constexpr uint16_t MQTT_PORT = 1883;  // Plain MQTT, no TLS.
// Topic: storeyes/<device_id>/coffee-counter. The broker ACL should allow each device only its own.
constexpr const char* MQTT_TOPIC_PREFIX = "storeyes/";
constexpr const char* MQTT_TOPIC_SUFFIX = "/coffee-counter";
constexpr int MQTT_QOS = 1;  // Publish waits for PUBACK. Subscribers dedupe on reading_id.
constexpr int MQTT_KEEPALIVE_SECONDS = 30;
constexpr int MQTT_TIMEOUT_MS = 4000;
constexpr size_t PAYLOAD_CAPACITY = 5120;  // Full track ~4.7 KB; a reading ~900 B. One shared buffer.
constexpr int MQTT_BUFFER_SIZE = PAYLOAD_CAPACITY + 128;  // Payload + topic + header must fit.

// Broker credentials, shared by all devices. Without TLS they are sent in plain text,
// and they are readable in the binary published to the public repository.
constexpr const char* MQTT_USERNAME = "storeyes";
constexpr const char* MQTT_PASSWORD = "12345";

// --------------------------------------------------
// OTA — plain HTTP from admin-panel nginx; the binary must match its published SHA-256
// --------------------------------------------------

constexpr const char* VERSION_URL =
  "http://18.100.207.236/coffee-counter/version.txt";

constexpr const char* FIRMWARE_URL =
  "http://18.100.207.236/coffee-counter/build/esp32.esp32.esp32c3/retail_ota.ino.bin";

constexpr const char* FIRMWARE_SHA256_URL =
  "http://18.100.207.236/coffee-counter/build/esp32.esp32.esp32c3/retail_ota.ino.bin.sha256";

constexpr uint32_t OTA_STALL_MS = 10000;  // No data for this long: download abandoned.
