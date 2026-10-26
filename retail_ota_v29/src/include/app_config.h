#pragma once

// All tunable constants for firmware 29. constexpr keeps each one private to the file
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
#define ENABLE_SERIAL_LOG 1
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

constexpr int CURRENT_VERSION = 29;
// Used only on FIRST provisioning. Subsequent OTA firmware keeps the stored ID.
// Before first use on another ESP, register it and replace this initial UUID.
constexpr const char* INITIAL_DEVICE_ID = "4f974379-a444-4c50-8cca-175d7803ceda";  // "e2a880ac-c44f-4708-b315-9b6a3eab727a";  //=> the comment is the test device ID version 26

// Dashboard calibration: 26.7 cm = 11 %, 27.0 cm = 10 %, 8.7 cm = 71 % (25 Oct readings).
constexpr bool CALIBRATION_CONFIRMED = true;
constexpr float DISTANCE_FULL_CM = 0.0f;
constexpr float DISTANCE_EMPTY_CM = 30.0f;
constexpr int CONFIG_VERSION = 2;
constexpr uint32_t NORMAL_INTERVAL = 600;  // Only without calibration.

// Wake schedule, tuned by replaying the 14-25 Oct readings (fastest drain seen ~11 %/h).
constexpr uint32_t REFILL_HOLD_INTERVAL = 4 * 3600;  // From ~76 % the level cannot reach 20 % in 4 h.
// >= 30 %: a fast drain (~11 %/h) can pass 20 % inside one hour; the replay saw it up to 28 min late.
constexpr uint32_t HIGH_LEVEL_INTERVAL = 3600;
constexpr uint32_t DRAINING_INTERVAL = 300;  // < 30 %: the refill is seen within 5 min.
constexpr uint32_t NIGHT_STABLE_INTERVAL = 3600;
constexpr float SCHEDULE_HIGH_LEVEL_PERCENT = 30.0f;
constexpr float REFILL_JUMP_PERCENT = 15.0f;
// The cafe is idle overnight, on the UTC clock the dashboard shows.
constexpr int NIGHT_START_HOUR_UTC = 21;
constexpr int NIGHT_END_HOUR_UTC = 6;
// Stable = moved < 1.5 %/h over >= 30 min; normal draw is ~6.7 %/h. Night only: a slow
// daytime drain judged stable delayed the 20 % crossing by up to 33 min in the replay.
constexpr float STABLE_RATE_PERCENT_PER_HOUR = 1.5f;
constexpr uint32_t STABLE_MIN_SPAN_MINUTES = 30;
constexpr int FILL_HISTORY = 12;            // 12 x 5 min covers the 30 min span.
constexpr uint32_t SENSOR_RETRY_WAKES = 3;  // Unreadable sensor: 5 min retries, then hourly.

constexpr uint32_t MIN_SLEEP_MS = 5000;
constexpr uint32_t NETWORK_WORK_BUDGET_MS = 45000;
constexpr uint64_t OTA_CHECK_INTERVAL_MS = 24ULL * 3600ULL * 1000ULL;

// The RTC slow clock is the internal RC oscillator; resync often enough to keep seconds accuracy.
constexpr uint64_t NTP_RESYNC_INTERVAL_MS = 2ULL * 3600ULL * 1000ULL;
constexpr uint32_t NTP_WAIT_MS = 1200;
constexpr time_t VALID_TIME_AFTER = 1735689600;  // 2025-01-01

// Timer wakes only; power-on/RESET always runs the full recovery path.
constexpr uint32_t WIFI_BACKOFF_AFTER_FAILED_WAKES = 3;
constexpr uint32_t WIFI_BACKOFF_INTERVAL = 3600;

constexpr unsigned long WIFI_CONNECT_TIMEOUT_SECONDS = 12;
// The cached AP must associate within this time or the full scan takes over.
constexpr uint32_t FAST_ASSOCIATE_TIMEOUT_MS = 2500;
constexpr unsigned long PORTAL_TIMEOUT_SECONDS = 180;

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

// The VL53L0X times 940 nm laser pulses. Dark beans return little light and ambient IR
// adds photon noise; a longer timing budget integrates more signal (sigma ~ 1/sqrt(budget)).
constexpr uint32_t RANGE_TIMING_BUDGET_US = 100000;
constexpr uint32_t RANGE_ESCALATED_BUDGET_US = 200000;  // ST "high accuracy" budget.
constexpr int RANGE_SAMPLE_COUNT = 5;                    // Per pass; at most two passes.
constexpr int RANGE_MIN_VALID_SAMPLES = 3;

constexpr uint16_t RANGE_MIN_MM = 20;
// The hopper is ~300 mm deep; longer returns are phase wrap or the beam passing the beans.
constexpr uint16_t RANGE_MAX_MM = 600;
// Per-sample sigma from the sensor; above this the return is dominated by noise.
constexpr float RANGE_SIGMA_MAX_MM = 12.0f;
// Inlier window around the median: max(this, 3 x scaled MAD). Rejects a falling bean or a hand.
constexpr float RANGE_OUTLIER_MIN_MM = 5.0f;
// Re-measure with the longer budget when the first pass looks optically weak.
constexpr int RANGE_ESCALATE_SPREAD_MM = 8;
constexpr float RANGE_ESCALATE_SIGMA_MM = 6.0f;
constexpr float DISTANCE_UNAVAILABLE_CM = 999.0f;

// VL53L0X supports fast mode; drop to 100000 if long wires or weak pull-ups fail init.
constexpr uint32_t I2C_CLOCK_HZ = 400000;

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
// MQTT — replaces the Supabase REST upload
// --------------------------------------------------

constexpr const char* MQTT_HOST = "18.100.207.236";
constexpr uint16_t MQTT_PORT = 1883;  // Plain MQTT, no TLS.
// Topic: storeyes/<device_id>/coffee-counter. The broker ACL should allow each device only its own.
constexpr const char* MQTT_TOPIC_PREFIX = "storeyes/";
constexpr const char* MQTT_TOPIC_SUFFIX = "/coffee-counter";
constexpr int MQTT_QOS = 1;  // Publish waits for PUBACK. Subscribers dedupe on reading_id.
constexpr int MQTT_KEEPALIVE_SECONDS = 30;
constexpr int MQTT_TIMEOUT_MS = 4000;
constexpr int MQTT_BUFFER_SIZE = 1024;  // Payload + topic + header must fit.

// Broker credentials, shared by all devices. Without TLS they are sent in plain text,
// and they are readable in the binary published to the public repository.
constexpr const char* MQTT_USERNAME = "storeyes";
constexpr const char* MQTT_PASSWORD = "12345";

constexpr size_t PAYLOAD_CAPACITY = 1280;  // Worst case ~970 bytes (30 fields in 8 groups).

// --------------------------------------------------
// OTA — existing repository paths
// --------------------------------------------------

constexpr const char* VERSION_URL =
  "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/version.txt";

constexpr const char* FIRMWARE_URL =
  "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/build/esp32.esp32.esp32c3/retail_ota.ino.bin";
