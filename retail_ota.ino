/*
 AI Coffee V4 / ESP32-C3 / firmware 26
 Based on verified retail_ota firmware 25; existing pins, ADC ratios, key and OTA paths retained.
 Device UUID provisioned from the user's successful database migration.

 BEFORE USE: set CALIBRATION_CONFIRMED and measured FULL/EMPTY distances below.
 Until then: raw distance uploads, fill_percent=null, fixed 600-second cycle.
 Linear fill is HEIGHT percent; tapered reservoirs need a calibrated volume model.

 New: RTC session/sequence/failure diagnostics, bounded Wi-Fi attempts, duplicate-safe
 upload retries, adaptive 600/300/60s start-to-start scheduling with hysteresis.
 Timed library calls can overrun the scheduling target; portal and OTA are exceptions.
 Counters survive deep sleep, not loss of power/reset; session UUID changes on reset.
 No offline measurement queue: failed uploads leave sequence gaps.
 Existing TLS setInsecure retained; per-device authentication is NOT implemented.
 Test via USB before publishing an OTA binary. Never publish version.txt first.
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <Ticker.h>
#include <time.h>
#include <ctype.h>
#include <Preferences.h>
#include <esp_random.h>
#include <Adafruit_VL53L0X.h>

#include <esp_sleep.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_err.h>
#include <esp_ota_ops.h>
#include <driver/gpio.h>

#if !defined(CONFIG_IDF_TARGET_ESP32C3)
#error "Select an ESP32-C3 board."
#endif

// --------------------------------------------------
// Firmware and timing
// --------------------------------------------------

constexpr int CURRENT_VERSION = 27;
// Used only on FIRST provisioning. Subsequent OTA firmware keeps the stored ID.
// Before first use on another ESP, register it and replace this initial UUID.
const char* INITIAL_DEVICE_ID = "4f974379-a444-4c50-8cca-175d7803ceda";  // "e2a880ac-c44f-4708-b315-9b6a3eab727a";  //=> the comment is the test device ID version 26

// Enter actual installed full/empty distances in cm; do not use the sensor's maximum range.
constexpr bool CALIBRATION_CONFIRMED = false;
constexpr float DISTANCE_FULL_CM = 0.0f;
constexpr float DISTANCE_EMPTY_CM = 0.0f;
constexpr int CONFIG_VERSION = 1;
constexpr uint32_t NORMAL_INTERVAL = 600;
constexpr uint32_t LOW_INTERVAL = 300;
constexpr uint32_t CRITICAL_INTERVAL = 60;
constexpr uint32_t MIN_SLEEP_MS = 5000;
constexpr uint32_t NETWORK_WORK_BUDGET_MS = 45000;
constexpr uint32_t HTTP_STAGE_TIMEOUT_MS = 4000;
constexpr uint32_t OTA_CHECK_INTERVAL_MS = 600000;

constexpr unsigned long WIFI_CONNECT_TIMEOUT_SECONDS = 12;
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

// Preserve v23 configuration. No XSHUT wiring change required.
constexpr int XSHUT_PIN = 6;
constexpr bool USE_XSHUT = false;

// --------------------------------------------------
// Distance measurement
// --------------------------------------------------

constexpr uint32_t RANGE_TIMING_BUDGET_US = 100000;
constexpr int RANGE_SAMPLE_COUNT = 5;
constexpr int RANGE_MIN_VALID_SAMPLES = 3;

constexpr uint16_t RANGE_MIN_MM = 20;
constexpr uint16_t RANGE_MAX_MM = 2000;
constexpr float DISTANCE_UNAVAILABLE_CM = 999.0f;

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
constexpr int8_t WIFI_TX_POWER_QUARTER_DBM = 34;

// --------------------------------------------------
// Supabase — retain your existing anon key
// --------------------------------------------------

const char* SUPABASE_URL =
  "https://yvgsorxwofgpkshlczlm.supabase.co/rest/v1/sensor_data";

const char* SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Inl2Z3Nvcnh3b2ZncGtzaGxjemxtIiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODIxOTc2ODgsImV4cCI6MjA5Nzc3MzY4OH0.MgrGm-zTR5DIv2rwmBH0S3rMEDUHA_0ol-Ej43lHBk8";

// --------------------------------------------------
// OTA — existing repository paths
// --------------------------------------------------

const char* VERSION_URL =
  "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/version.txt";

const char* FIRMWARE_URL =
  "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/build/esp32.esp32.esp32c3/retail_ota.ino.bin";

// --------------------------------------------------
// Objects and state
// --------------------------------------------------

Adafruit_VL53L0X lox;
Ticker ticker;

bool sensorReady = false;

// POD only: retained across deep sleep; invalidated on any other reset or firmware change.
struct RetainedState {
  uint32_t magic;
  char session[37];
  uint64_t sequence;
  uint64_t wakes;
  uint64_t wifiFailures;
  uint64_t uploadFailures;
  uint32_t previousAwakeMs;
  int previousHttp;
  uint32_t interval;
  uint64_t elapsedMs;
  uint64_t nextOtaMs;
};
RTC_DATA_ATTR RetainedState retained;
constexpr uint32_t RTC_MAGIC = 0xC0FF0026;
uint32_t cycleStartMs = 0;
uint32_t networkStartMs = 0;
uint32_t wifiConnectMs = 0;
uint32_t sampleTakenMs = 0;
int sampleCount = 0;
int sampleSpread = -1;
const char* sensorError = "not_measured";
float fillPercent = -1;
uint32_t selectedInterval = NORMAL_INTERVAL;
bool portalUsed = false;
bool timerWake = false;
char readingId[37] = {};
char deviceId[37] = {};

bool validUuid(const String& value) {
  if (value.length() != 36) return false;
  for (int i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') return false;
    } else if (!isxdigit((unsigned char)value[i])) return false;
  }
  return true;
}

bool loadDeviceIdentity() {
  Preferences prefs;
  if (!prefs.begin("coffee-id", false)) return false;
  String stored = prefs.getString("device_id", "");
  if (stored.isEmpty()) {
    stored = INITIAL_DEVICE_ID;
    if (!validUuid(stored) || prefs.putString("device_id", stored) == 0) {
      prefs.end();
      return false;
    }
  }
  prefs.end();
  if (!validUuid(stored)) return false;
  stored.toCharArray(deviceId, sizeof(deviceId));
  return true;
}

void makeUuid(char* output) {
  // Called only with the Wi-Fi radio started, for hardware entropy.
  uint8_t b[16];
  esp_fill_random(b, sizeof(b));
  b[6] = (b[6] & 0x0f) | 0x40;
  b[8] = (b[8] & 0x3f) | 0x80;
  snprintf(output, 37,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

bool calibrationValid() {
  return CALIBRATION_CONFIRMED && isfinite(DISTANCE_FULL_CM)
         && isfinite(DISTANCE_EMPTY_CM) && DISTANCE_FULL_CM >= 0
         && DISTANCE_EMPTY_CM > DISTANCE_FULL_CM;
}

uint32_t chooseInterval(float distance) {
  fillPercent = -1;
  if (!calibrationValid()) return NORMAL_INTERVAL;
  if (distance == DISTANCE_UNAVAILABLE_CM) {
    return retained.interval == CRITICAL_INTERVAL ? CRITICAL_INTERVAL : LOW_INTERVAL;
  }
  fillPercent = constrain(100.0f * (DISTANCE_EMPTY_CM - distance)
                            / (DISTANCE_EMPTY_CM - DISTANCE_FULL_CM),
                          0.0f, 100.0f);
  // Faster immediately; slower only after crossing recovery thresholds.
  if (fillPercent < 20.0f) return CRITICAL_INTERVAL;
  if (retained.interval == CRITICAL_INTERVAL && fillPercent < 23.0f)
    return CRITICAL_INTERVAL;
  if (fillPercent < 50.0f) return LOW_INTERVAL;
  if (retained.interval != NORMAL_INTERVAL && fillPercent < 53.0f)
    return LOW_INTERVAL;
  return NORMAL_INTERVAL;
}

bool networkBudgetAvailable(uint32_t reserveMs = 0) {
  return (uint32_t)(millis() - networkStartMs) + reserveMs < NETWORK_WORK_BUDGET_MS;
}

void goToDeepSleep();

// --------------------------------------------------
// LED
// --------------------------------------------------

void setLed(bool on) {
  if (LED_PIN < 0) return;

  digitalWrite(
    LED_PIN,
    (on != LED_ACTIVE_LOW) ? HIGH : LOW);
}

void tick() {
  if (LED_PIN >= 0) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
}

void stopLedBlink() {
  ticker.detach();
  setLed(false);
}

void startLedBlink(float seconds) {
  ticker.detach();
  setLed(true);

  if (LED_PIN >= 0) {
    ticker.attach(seconds, tick);
  }
}

void initializeLed() {
  if (LED_PIN < 0) return;

  // Release the OFF state held during the previous deep sleep.
  gpio_hold_dis((gpio_num_t)LED_PIN);
  gpio_deep_sleep_hold_dis();

  pinMode(LED_PIN, OUTPUT);
  setLed(false);
}

void flashUploadError() {
  stopLedBlink();

  for (int i = 0; i < 3; ++i) {
    setLed(true);
    delay(70);
    setLed(false);
    delay(70);
  }
}

// --------------------------------------------------
// Optional sensor shutdown
// --------------------------------------------------

void prepareSensorShutdown() {
  if (!USE_XSHUT) return;

  pinMode(XSHUT_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(XSHUT_PIN, LOW);

  gpio_hold_dis((gpio_num_t)XSHUT_PIN);
  gpio_deep_sleep_hold_dis();

  digitalWrite(XSHUT_PIN, LOW);
}

void wakeSensor() {
  if (!USE_XSHUT) return;

  // Open-drain HIGH releases the pin to the breakout pull-up.
  digitalWrite(XSHUT_PIN, HIGH);
  delay(10);
}

void shutDownSensor() {
  if (!USE_XSHUT) return;

  digitalWrite(XSHUT_PIN, LOW);
  sensorReady = false;
}

// --------------------------------------------------
// Deep sleep
// --------------------------------------------------

void goToDeepSleep() {
  stopLedBlink();
  shutDownSensor();

  // Disable the radio without deleting saved credentials.
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);

  // Hold built-in active-low LED OFF throughout deep sleep.
  if (LED_PIN >= 0) {
    setLed(false);

    esp_err_t err = gpio_hold_en((gpio_num_t)LED_PIN);
    if (err != ESP_OK) {
      Serial.printf(
        "LED hold failed: %s\n",
        esp_err_to_name(err));
    }
  }

  if (USE_XSHUT) {
    esp_err_t err = gpio_hold_en((gpio_num_t)XSHUT_PIN);
    if (err != ESP_OK) {
      Serial.printf(
        "XSHUT hold failed: %s\n",
        esp_err_to_name(err));
    }
  }

  gpio_deep_sleep_hold_en();

  uint32_t awakeMs = millis() - cycleStartMs;
  uint32_t targetMs = selectedInterval * 1000UL;
  uint32_t sleepMs = awakeMs < targetMs ? targetMs - awakeMs : MIN_SLEEP_MS;
  if (sleepMs < MIN_SLEEP_MS) sleepMs = MIN_SLEEP_MS;
  retained.previousAwakeMs = awakeMs;
  retained.interval = selectedInterval;
  retained.elapsedMs += (uint64_t)awakeMs + sleepMs;
  Serial.printf("Awake: %lu ms; target interval: %lu s; sleep: %lu ms\n",
                (unsigned long)awakeMs, (unsigned long)selectedInterval, (unsigned long)sleepMs);
  esp_sleep_enable_timer_wakeup((uint64_t)sleepMs * 1000ULL);

  Serial.flush();
  esp_deep_sleep_start();
}

// --------------------------------------------------
// Wi-Fi power and callbacks
// --------------------------------------------------

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_START || event == ARDUINO_EVENT_WIFI_AP_START) {

    esp_err_t err =
      esp_wifi_set_max_tx_power(WIFI_TX_POWER_QUARTER_DBM);

    Serial.printf(
      "Wi-Fi %s start: 8.5 dBm request %s\n",
      event == ARDUINO_EVENT_WIFI_AP_START ? "AP" : "STA",
      esp_err_to_name(err));

  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf(
      "Wi-Fi disconnected; reason code: %u\n",
      (unsigned)info.wifi_sta_disconnected.reason);
  }
}

bool applyWiFiPower(const char* stage) {
  bool ok = WiFi.setTxPower(WIFI_TX_POWER);

  Serial.printf(
    "Wi-Fi power [%s]: %s; reported limit %.2f dBm\n",
    stage,
    ok ? "accepted" : "FAILED",
    (int)WiFi.getTxPower() / 4.0f);

  return ok;
}

void configModeCallback(WiFiManager*) {
  ticker.detach();
  setLed(true);

  if (!applyWiFiPower("configuration portal")) {
    Serial.println("Portal TX power failed; sleeping.");
    goToDeepSleep();
    return;
  }

  Serial.println("Wi-Fi portal: AI Coffee / 12345678");
  Serial.print("Open http://");
  Serial.println(WiFi.softAPIP());
  Serial.println("Portal timeout: 180 seconds, then sleep.");
}

void credentialsSavedCallback() {
  Serial.println(
    "Wi-Fi settings saved; continuing without an extra reboot.");
}

bool connectWiFi() {
  startLedBlink(0.2f);
  networkStartMs = millis();
  WiFi.onEvent(onWiFiEvent);
  WiFi.setAutoReconnect(false);
  if (!WiFi.mode(WIFI_STA) || !applyWiFiPower("before connection")) {
    ++retained.wifiFailures;
    stopLedBlink();
    return false;
  }
  if (!retained.session[0]) makeUuid(retained.session);

  bool connected = false;
  if (!FORCE_CONFIG_PORTAL) {
    for (int attempt = 1; attempt <= 2; ++attempt) {
      Serial.printf("Wi-Fi attempt %d/2\n", attempt);
      WiFi.begin();  // Stored credentials; never erase them on a transient failure.
      uint32_t start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_SECONDS * 1000UL) delay(50);
      if (WiFi.status() == WL_CONNECTED) {
        connected = true;
        break;
      }
      ++retained.wifiFailures;
      WiFi.disconnect(false, false);
      delay(250);
    }
  }
  // Power-on / RESET is the explicit recovery path for changed routers.
  // Timer wakes never waste three minutes in an unattended setup portal.
  if (!connected && (!timerWake || FORCE_CONFIG_PORTAL)) {
    portalUsed = true;
    WiFiManager wm;
    wm.setDebugOutput(false);
    wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT_SECONDS);
    wm.setSaveConnectTimeout(WIFI_CONNECT_TIMEOUT_SECONDS);
    wm.setConfigPortalTimeout(PORTAL_TIMEOUT_SECONDS);
    wm.setAPClientCheck(false);
    wm.setWebPortalClientCheck(false);  // Connected phone must not extend the portal forever.
    wm.setAPCallback(configModeCallback);
    wm.setSaveConfigCallback(credentialsSavedCallback);
    connected = wm.startConfigPortal("AI Coffee", "12345678");
    if (!connected) ++retained.wifiFailures;
  }
  wifiConnectMs = millis() - networkStartMs;
  stopLedBlink();
  if (!connected || WiFi.status() != WL_CONNECTED) return false;
  if (!applyWiFiPower("connected")) {
    ++retained.wifiFailures;
    return false;
  }
  if (portalUsed) networkStartMs = millis();  // Portal is an explicit scheduling exception.
  Serial.printf("Connected: %s; RSSI %ld dBm; connect %lu ms\n",
                WiFi.localIP().toString().c_str(), (long)WiFi.RSSI(), (unsigned long)wifiConnectMs);
  return true;
}

// --------------------------------------------------
// ADC measurements — v23 method
// --------------------------------------------------

float median3(float a, float b, float c) {
  if (a > b) {
    float t = a;
    a = b;
    b = t;
  }
  if (b > c) {
    float t = b;
    b = c;
    c = t;
  }
  if (a > b) {
    float t = a;
    a = b;
    b = t;
  }

  return b;
}

float readPinVoltage(int pin) {
  // Discard first reading after changing ADC channels.
  analogReadMilliVolts(pin);
  delay(10);

  float values[3];

  for (int i = 0; i < 3; ++i) {
    values[i] = analogReadMilliVolts(pin) / 1000.0f;
    delay(10);
  }

  return median3(values[0], values[1], values[2]);
}

float readBattery() {
  float pinV = readPinVoltage(BATTERY_PIN);

  float batteryV =
    pinV * (1.0f + BATTERY_R_TOP / BATTERY_R_BOTTOM)
      * BATTERY_GAIN
    + BATTERY_OFFSET_V;

  Serial.printf(
    "Battery ADC: %.3f V; reported battery: %.3f V\n",
    pinV,
    batteryV);

  return batteryV;
}

bool readPluginStatus() {
  float pinV = readPinVoltage(PLUGIN_PIN);

  float inputV =
    pinV * (1.0f + USB_R_TOP / USB_R_BOTTOM);

  Serial.printf(
    "USB ADC: %.3f V; estimated charger input: %.3f V\n",
    pinV,
    inputV);

  return inputV >= USB_PRESENT_THRESHOLD_V;
}

// --------------------------------------------------
// VL53L0X — v23 five-sample median
// --------------------------------------------------

bool initializeSensor() {
  wakeSensor();

  sensorReady = lox.begin(0x29, false, &Wire);

  if (sensorReady) {
    sensorReady =
      lox.setMeasurementTimingBudgetMicroSeconds(
        RANGE_TIMING_BUDGET_US);
  }

  Serial.println(
    sensorReady
      ? "VL53L0X ready, 100 ms timing budget."
      : "VL53L0X initialization failed; distance will be 999.");

  return sensorReady;
}

float readDistance() {
  sampleCount = 0;
  sampleSpread = -1;
  sensorError = "initialization_failed";
  if (!sensorReady) {
    return DISTANCE_UNAVAILABLE_CM;
  }

  uint16_t valid[RANGE_SAMPLE_COUNT];
  int count = 0;

  Serial.println(
    "Distance samples: mm / range status / API status");

  for (int i = 0; i < RANGE_SAMPLE_COUNT; ++i) {
    VL53L0X_RangingMeasurementData_t measurement = {};

    VL53L0X_Error error =
      lox.getSingleRangingMeasurement(&measurement, false);

    bool accepted =
      error == VL53L0X_ERROR_NONE && measurement.RangeStatus == 0 && measurement.RangeMilliMeter > RANGE_MIN_MM && measurement.RangeMilliMeter < RANGE_MAX_MM;

    Serial.printf(
      "  %d: %u / %u / %d %s\n",
      i + 1,
      (unsigned)measurement.RangeMilliMeter,
      (unsigned)measurement.RangeStatus,
      (int)error,
      accepted ? "OK" : "rejected");

    if (accepted) {
      valid[count++] = measurement.RangeMilliMeter;
    }

    delay(20);
  }

  sampleCount = count;
  sensorError = "insufficient_valid_samples";
  if (count < RANGE_MIN_VALID_SAMPLES) {
    Serial.printf(
      "Distance unavailable: only %d/%d valid samples.\n",
      count,
      RANGE_SAMPLE_COUNT);

    return DISTANCE_UNAVAILABLE_CM;
  }

  for (int i = 1; i < count; ++i) {
    uint16_t value = valid[i];
    int j = i - 1;

    while (j >= 0 && valid[j] > value) {
      valid[j + 1] = valid[j];
      --j;
    }

    valid[j + 1] = value;
  }

  float medianMM = (count % 2)
                     ? valid[count / 2]
                     : (valid[count / 2 - 1] + valid[count / 2]) / 2.0f;

  Serial.printf(
    "Median: %.1f mm; valid: %d/%d; spread: %u mm\n",
    medianMM,
    count,
    RANGE_SAMPLE_COUNT,
    (unsigned)(valid[count - 1] - valid[0]));

  sampleSpread = valid[count - 1] - valid[0];
  sensorError = "none";
  return medianMM / 10.0f;
}

// --------------------------------------------------
// Supabase
// --------------------------------------------------

String measurementTimestamp() {
  // Synchronize after Wi-Fi, then reconstruct the earlier measurement time.
  // An invalid clock produces null, never a made-up server/measurement timestamp.
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  uint32_t start = millis();
  time_t now = time(nullptr);
  while (now < 1735689600 && millis() - start < 1200 && networkBudgetAvailable(15000)) {
    delay(50);
    now = time(nullptr);
  }
  if (now < 1735689600) return "null";
  time_t measured = now - (time_t)((millis() - sampleTakenMs) / 1000UL);
  struct tm utc;
  gmtime_r(&measured, &utc);
  char stamp[32];
  strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return String("\"") + stamp + "\"";
}

bool sendDataToSupabase(float distance, float battery, bool plugged) {
  // Freeze BOTH UUID and payload before retrying. Retry diagnostics appear next wake.
  makeUuid(readingId);
  String measuredAt = measurementTimestamp();
  String payload;
  payload.reserve(1200);
  payload = String("{\"distance\":") + String(distance, 2)
            + ",\"battery_voltage\":" + String(battery, 3)
            + ",\"is_plugged\":" + (plugged ? "true" : "false")
            + ",\"firmware_version\":26"
            + ",\"device_id\":\"" + deviceId + "\""
            + ",\"reading_id\":\"" + readingId + "\""
            + ",\"session_id\":\"" + retained.session + "\"";
  char counters[320];
  snprintf(counters, sizeof(counters),
           ",\"sequence_no\":%llu,\"wake_count\":%llu,\"wifi_failures_total\":%llu,\"upload_failures_total\":%llu",
           (unsigned long long)retained.sequence, (unsigned long long)retained.wakes,
           (unsigned long long)retained.wifiFailures, (unsigned long long)retained.uploadFailures);
  payload += counters;
  payload += ",\"measured_at\":" + measuredAt
             + ",\"fill_percent\":" + (fillPercent >= 0 ? String(fillPercent, 2) : String("null"))
             + ",\"interval_seconds\":" + String(selectedInterval)
             + ",\"config_version\":" + (calibrationValid() ? String(CONFIG_VERSION) : String("null"))
             + ",\"sensor_valid\":" + (distance != DISTANCE_UNAVAILABLE_CM ? "true" : "false")
             + ",\"valid_samples\":" + String(sampleCount)
             + ",\"sample_spread_mm\":" + (sampleSpread >= 0 ? String(sampleSpread) : String("null"))
             + ",\"sensor_error\":" + (distance != DISTANCE_UNAVAILABLE_CM ? String("null") : String("\"") + sensorError + "\"")
             + ",\"wifi_rssi\":" + String(WiFi.RSSI())
             + ",\"wifi_connect_ms\":" + String(wifiConnectMs)
             + ",\"reset_reason\":" + String((int)esp_reset_reason())
             + ",\"wakeup_cause\":" + String((int)esp_sleep_get_wakeup_cause())
             + ",\"previous_upload_http_status\":" + (retained.previousHttp ? String(retained.previousHttp) : String("null"))
             + ",\"previous_awake_ms\":" + (retained.previousAwakeMs ? String(retained.previousAwakeMs) : String("null")) + "}";

  retained.previousHttp = 0;
  Serial.printf("Reading %s; session %s; sequence %llu\n", readingId,
                retained.session, (unsigned long long)retained.sequence);
  for (int attempt = 1; attempt <= 3; ++attempt) {
    // Stage limits are not a guaranteed total request deadline.
    if (WiFi.status() != WL_CONNECTED || !networkBudgetAvailable(12000)) break;
    WiFiClientSecure client;
    client.setInsecure();  // Preserved from v25; review before production deployment.
    client.setHandshakeTimeout(4);
    HTTPClient http;
    http.setConnectTimeout(HTTP_STAGE_TIMEOUT_MS);
    http.setTimeout(HTTP_STAGE_TIMEOUT_MS);
    String endpoint = String(SUPABASE_URL) + "?on_conflict=reading_id";
    if (!http.begin(client, endpoint)) {
      ++retained.uploadFailures;
      break;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", SUPABASE_ANON_KEY);
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
    http.addHeader("Prefer", "resolution=ignore-duplicates,return=minimal");
    int code = http.POST(payload);
    retained.previousHttp = code;
    Serial.printf("Supabase attempt %d/3: HTTP %d\n", attempt, code);
    if (code >= 200 && code < 300) {
      http.end();
      return true;
    }
    if (code > 0) Serial.println(http.getString().substring(0, 500));
    else Serial.println(http.errorToString(code));
    http.end();
    ++retained.uploadFailures;
    bool transient = code < 0 || code == 408 || code == 429 || code >= 500;
    if (!transient) break;  // No repeated schema, credentials or FK errors.
    if (attempt < 3 && networkBudgetAvailable(13000)) delay(500 * attempt);
  }
  Serial.println("Reading not acknowledged; no offline queue. Next sequence will advance.");
  return false;
}

// --------------------------------------------------
// OTA diagnostics and update
// --------------------------------------------------

void printOTAPartitions() {
  const esp_partition_t* running =
    esp_ota_get_running_partition();

  const esp_partition_t* target =
    esp_ota_get_next_update_partition(nullptr);

  Serial.printf(
    "[OTA] Firmware=%d; build=%s %s; sketch=%lu bytes\n",
    CURRENT_VERSION,
    __DATE__,
    __TIME__,
    (unsigned long)ESP.getSketchSize());

  if (running) {
    Serial.printf(
      "[OTA] Running partition=%s size=%lu\n",
      running->label,
      (unsigned long)running->size);
  }

  if (target) {
    Serial.printf(
      "[OTA] Next partition=%s size=%lu\n",
      target->label,
      (unsigned long)target->size);
  } else {
    Serial.println("[OTA] No update partition available.");
  }
}

int fetchLatestVersion() {
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(4);

  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  String url =
    String(VERSION_URL)
    + "?check="
    + String((unsigned long)esp_random());

  Serial.printf("[OTA] Version URL: %s\n", VERSION_URL);

  if (!http.begin(client, url)) {
    Serial.println("[OTA] HTTP initialization failed.");
    return -1;
  }

  http.addHeader("Cache-Control", "no-cache");

  int code = http.GET();
  Serial.printf("[OTA] Version HTTP status=%d\n", code);

  if (code != HTTP_CODE_OK) {
    if (code < 0) {
      Serial.println(http.errorToString(code));
    }

    if (code == 404) {
      Serial.println(
        "[OTA] Check root version.txt on the main branch.");
    }

    http.end();
    return -1;
  }

  String value = http.getString();
  http.end();

  if (value.startsWith("\xEF\xBB\xBF")) {
    value.remove(0, 3);
  }

  value.trim();

  if (value.isEmpty() || value.length() > 9) {
    Serial.println("[OTA] Invalid version.txt: use an integer.");
    return -1;
  }

  for (unsigned int i = 0; i < value.length(); ++i) {
    if (value[i] < '0' || value[i] > '9') {
      Serial.println("[OTA] Use an integer, not v26, JSON, or HTML.");
      return -1;
    }
  }

  int latest = value.toInt();

  if (latest <= 0) {
    return -1;
  }

  Serial.printf(
    "[OTA] Installed=%d; published=%d\n",
    CURRENT_VERSION,
    latest);

  return latest;
}

void onOTAProgress(int current, int total) {
  static int lastBucket = -1;

  int percent = total > 0
                  ? (int)((int64_t)current * 100 / total)
                  : 0;

  int bucket = percent / 10;

  if (bucket != lastBucket || current == total) {
    Serial.printf(
      "[OTA] Download: %d/%d bytes (%d%%)\n",
      current,
      total,
      percent);

    lastBucket = bucket;
  }
}

void checkForUpdates() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[OTA] Skipped: Wi-Fi disconnected.");
    return;
  }

  printOTAPartitions();
  startLedBlink(0.10f);

  int latest = fetchLatestVersion();

  if (latest <= CURRENT_VERSION) {
    Serial.println(
      latest > 0
        ? "[OTA] No newer firmware."
        : "[OTA] Check failed; retry next wake.");

    stopLedBlink();
    return;
  }

  const esp_partition_t* target =
    esp_ota_get_next_update_partition(nullptr);

  if (!target) {
    Serial.println(
      "[OTA] No OTA slot. USB flash with an OTA-capable layout.");

    stopLedBlink();
    return;
  }

  Serial.printf(
    "[OTA] Updating %d -> %d\n",
    CURRENT_VERSION,
    latest);
  Serial.printf("[OTA] Binary URL: %s\n", FIRMWARE_URL);

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(4);

  String binaryURL =
    String(FIRMWARE_URL)
    + "?version=" + String(latest)
    + "&check=" + String((unsigned long)esp_random());

  httpUpdate.rebootOnUpdate(false);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.onProgress(onOTAProgress);

  t_httpUpdate_return result =
    httpUpdate.update(client, binaryURL);

  stopLedBlink();

  if (result == HTTP_UPDATE_OK) {
    Serial.println("[OTA] Install succeeded; rebooting.");
    Serial.flush();
    ESP.restart();

  } else if (result == HTTP_UPDATE_FAILED) {
    Serial.printf(
      "[OTA] Failed (%d): %s\n",
      httpUpdate.getLastError(),
      httpUpdate.getLastErrorString().c_str());

    flashUploadError();

  } else {
    Serial.println("[OTA] Server returned no update.");
  }
}

// --------------------------------------------------
// Main cycle — v26 diagnostics and adaptive timing, established measurement order
// --------------------------------------------------

void setup() {
  cycleStartMs = millis();
  Serial.begin(115200);
  delay(300);  // No dependence on a USB host.
  initializeLed();
  prepareSensorShutdown();
  timerWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
  if (esp_reset_reason() != ESP_RST_DEEPSLEEP || retained.magic != RTC_MAGIC) {
    memset(&retained, 0, sizeof(retained));
    retained.magic = RTC_MAGIC;
    retained.interval = NORMAL_INTERVAL;
  }
  if (!loadDeviceIdentity()) {
    Serial.println("Device identity storage failed; upload skipped. Check NVS/provisioning.");
    goToDeepSleep();
    return;
  }
  ++retained.wakes;
  ++retained.sequence;  // One logical measurement per wake, including failures.
  Serial.printf("\nAI Coffee ESP32-C3 firmware %d; device %s\n", CURRENT_VERSION, deviceId);
  Serial.printf("Reset %d; wake cause %d; wake %llu\n", (int)esp_reset_reason(),
                (int)esp_sleep_get_wakeup_cause(), (unsigned long long)retained.wakes);
  if (!calibrationValid()) Serial.println("Calibration not confirmed: fixed 600s, fill_percent=null.");

  startLedBlink(0.2f);
  pinMode(BATTERY_PIN, INPUT);
  pinMode(PLUGIN_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
  analogSetPinAttenuation(PLUGIN_PIN, ADC_11db);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(100);
  initializeSensor();
  float distance = readDistance();
  if (distance == DISTANCE_UNAVAILABLE_CM) {
    Serial.println("Retrying sensor once.");
    delay(50);
    if (!sensorReady) initializeSensor();
    distance = readDistance();
  }
  sampleTakenMs = millis();
  shutDownSensor();
  float battery = readBattery();
  bool plugged = readPluginStatus();
  selectedInterval = chooseInterval(distance);
  Serial.printf("Distance %.2f cm; fill %.2f (negative=unknown); interval %lu s\n",
                distance, fillPercent, (unsigned long)selectedInterval);

  if (!connectWiFi()) {
    retained.previousHttp = 0;  // No upload attempted during this wake.
    Serial.println("Wi-Fi unavailable; sleeping. RESET opens setup after connection attempts.");
    goToDeepSleep();
    return;
  }
  startLedBlink(0.10f);
  bool sent = sendDataToSupabase(distance, battery, plugged);
  stopLedBlink();
  if (!sent) flashUploadError();

  uint64_t elapsed = retained.elapsedMs + (millis() - cycleStartMs);
  if (elapsed >= retained.nextOtaMs && networkBudgetAvailable(12000)) {
    retained.nextOtaMs = elapsed + OTA_CHECK_INTERVAL_MS;
    checkForUpdates();  // May exceed reporting interval when downloading firmware.
  }
  goToDeepSleep();
}

void loop() {
  goToDeepSleep();
}
