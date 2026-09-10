/*
 AI Coffee ESP32-C3 -- firmware 22, continuous diagnostic test.
 Target: same Arduino-ESP32 3.x and libraries as working firmware 21.
 No deep sleep. Five-sample median batch + ADC readings + one Supabase POST
 target every 2000 ms, measured start-to-start. Blocking sensor/network work
 can exceed this interval; log overruns and skip missed slots (no bursts).
 OTA check once at boot; successful OTA still installs and reboots.
 Runs until you stop/power off/reflash; no automatic 45-minute cutoff.

 CURRENT BREADBOARD (no electronics changes):
 VL53L0X VIN=3V3, GND=GND, SDA=GPIO4, SCL=GPIO5; XSHUT unused.
 Battery positive -> 10k -> GPIO0 -> 10k -> GND (multiplier 2).
 External USB INPUT 5V -> 10k -> GPIO1 -> 10k -> GND (multiplier 2).
 USB ADC is near its measurement ceiling at 5V: presence only, not precise
 USB-voltage measurement. Match these constants if actual resistors differ.
 Optional external active-high LED GPIO7; set LED_PIN=-1 if absent.

 Distance remains cm, 999=unavailable; battery is volts, not calibrated SOC.
 is_plugged means external supply present, not active charging.
 Same four database fields; firmware_version=22 identifies diagnostic rows.
 Existing Supabase anon key and OTA URLs retained. TLS setInsecure retained
 from v21; server authentication is not implemented. Never use service_role.
 Failed/offline uploads are counted and dropped; no automatic POST retries.
 Faster Wi-Fi operation is NOT representative of ten-minute sleep runtime.
 Source reviewed only; not compiled or hardware-tested in this environment.
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <Ticker.h>
#include <Adafruit_VL53L0X.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_err.h>
#include <driver/gpio.h>
#include <limits.h>

#if !defined(CONFIG_IDF_TARGET_ESP32C3)
#error "Select an ESP32-C3 board. This sketch is for ESP32-C3 only."
#endif

constexpr int BATTERY_PIN = 0;
constexpr int PLUGIN_PIN = 1;
constexpr int SDA_PIN = 4;
constexpr int SCL_PIN = 5;
constexpr int XSHUT_PIN = 6;
// Optional: wire XSHUT and verify its pull-up before setting this true.
// Hardware standby is NOT guaranteed to reduce total breakout current.
constexpr bool USE_XSHUT = false;
constexpr uint32_t RANGE_TIMING_BUDGET_US = 100000;
constexpr int RANGE_SAMPLE_COUNT = 5;
constexpr int RANGE_MIN_VALID_SAMPLES = 3;
// Broad screening bounds, NOT calibrated hopper full/empty distances.
constexpr uint16_t RANGE_MIN_MM = 20;
constexpr uint16_t RANGE_MAX_MM = 2000;
constexpr int LED_PIN = 7; // EXTERNAL active-high LED; -1 disables it
constexpr uint32_t SAMPLE_INTERVAL_MS = 2000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
constexpr bool CHECK_OTA_AT_BOOT = true;
constexpr int CURRENT_VERSION = 22;
constexpr wifi_power_t WIFI_TX_POWER = WIFI_POWER_8_5dBm;
constexpr int8_t WIFI_TX_POWER_QUARTER_DBM = 34; // 8.5 dBm
constexpr unsigned long WIFI_CONNECT_TIMEOUT_SECONDS = 30;
constexpr unsigned long PORTAL_TIMEOUT_SECONDS = 300;
// Set true only to deliberately reopen setup even with saved credentials.
constexpr bool FORCE_CONFIG_PORTAL = false;

constexpr float BATTERY_R_TOP = 10000.0f;
constexpr float BATTERY_R_BOTTOM = 10000.0f;
constexpr float USB_R_TOP = 10000.0f;
constexpr float USB_R_BOTTOM = 10000.0f;
constexpr float BATTERY_GAIN = 1.0f;
constexpr float BATTERY_OFFSET_V = 0.0f;
constexpr float USB_PRESENT_THRESHOLD_V = 4.0f;

const char* SUPABASE_URL =
  "https://yvgsorxwofgpkshlczlm.supabase.co/rest/v1/sensor_data";
const char* SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Inl2Z3Nvcnh3b2ZncGtzaGxjemxtIiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODIxOTc2ODgsImV4cCI6MjA5Nzc3MzY4OH0.MgrGm-zTR5DIv2rwmBH0S3rMEDUHA_0ol-Ej43lHBk8";
const char* VERSION_URL =
  "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/firmware/esp32c3/version.txt";
const char* FIRMWARE_URL =
  "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/firmware/esp32c3/coffee_c3.bin";

Adafruit_VL53L0X lox;
Ticker ticker;
bool sensorReady = false;
// LOW asserts shutdown; HIGH in open-drain mode RELEASES the pin.
// No ESP32 internal pull-up: the breakout provides the correct logic voltage.
void prepareSensorShutdown() {
  if (!USE_XSHUT) return;
  // Configure the desired LOW state before releasing a previous sleep hold.
  pinMode(XSHUT_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(XSHUT_PIN, LOW);
  gpio_hold_dis((gpio_num_t)XSHUT_PIN);
  gpio_deep_sleep_hold_dis();
  digitalWrite(XSHUT_PIN, LOW);
}

void wakeSensor() {
  if (!USE_XSHUT) return;
  digitalWrite(XSHUT_PIN, HIGH); // Release to the breakout's pull-up
  delay(10); // Allow boot before lox.begin() reinitializes the sensor
}

void shutDownSensor() {
  if (!USE_XSHUT) return;
  digitalWrite(XSHUT_PIN, LOW);
  sensorReady = false;
}

// These event callbacks run on the network event task. Do not manipulate
// WiFiManager, ticker, or application state here.
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_START ||
      event == ARDUINO_EVENT_WIFI_AP_START) {
    // WiFiManager may stop/start the driver during provisioning.
    // Reapply the proven power limit each time an interface starts.
    esp_err_t err = esp_wifi_set_max_tx_power(WIFI_TX_POWER_QUARTER_DBM);
    Serial.printf("Wi-Fi %s start: 8.5 dBm request %s\n",
                  event == ARDUINO_EVENT_WIFI_AP_START ? "AP" : "STA",
                  esp_err_to_name(err));
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf("Wi-Fi disconnected; reason code: %u\n",
                  (unsigned)info.wifi_sta_disconnected.reason);
  }
}

bool applyWiFiPower(const char* stage) {
  bool ok = WiFi.setTxPower(WIFI_TX_POWER);
  Serial.printf("Wi-Fi power [%s]: %s; reported limit %.2f dBm\n",
                stage, ok ? "accepted" : "FAILED",
                (int)WiFi.getTxPower() / 4.0f);
  return ok;
}

void setLed(bool on) {
  if (LED_PIN >= 0) digitalWrite(LED_PIN, on ? HIGH : LOW);
}

void tick() {
  if (LED_PIN >= 0) digitalWrite(LED_PIN, !digitalRead(LED_PIN));
}

void stopOnWiFiPowerError() {
  ticker.detach();
  setLed(false);
  WiFi.mode(WIFI_OFF);
  Serial.println("Wi-Fi power configuration failed. Stopped; press RESET to retry.");
  while (true) delay(1000);
}

void configModeCallback(WiFiManager*) {
  ticker.detach();
  setLed(true);
  if (!applyWiFiPower("configuration portal")) {
    Serial.println("Portal TX power failed.");
    stopOnWiFiPowerError();
  }
  Serial.println("Wi-Fi portal: AI Coffee / 12345678");
  Serial.print("Open http://");
  Serial.println(WiFi.softAPIP());
  Serial.println("Portal timeout: 5 minutes; then continue offline with reconnect attempts.");
}

void credentialsSavedCallback() {
  Serial.println("Wi-Fi settings saved; continuing without an extra reboot.");
}

float median3(float a, float b, float c) {
  if (a > b) { float t = a; a = b; b = t; }
  if (b > c) { float t = b; b = c; c = t; }
  if (a > b) { float t = a; a = b; b = t; }
  return b;
}

float readPinVoltage(int pin) {
  analogReadMilliVolts(pin); // Discard first sample after ADC mux change
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
  Serial.printf("Battery ADC: %.3f V\n", pinV);
  return pinV * (1.0f + BATTERY_R_TOP / BATTERY_R_BOTTOM)
         * BATTERY_GAIN + BATTERY_OFFSET_V;
}

bool readPluginStatus() {
  float pinV = readPinVoltage(PLUGIN_PIN);
  float inputV = pinV * (1.0f + USB_R_TOP / USB_R_BOTTOM);
  Serial.printf("USB ADC: %.3f V; estimated charger input: %.3f V\n", pinV, inputV);
  return inputV >= USB_PRESENT_THRESHOLD_V;
}

float readDistance() {
  if (!sensorReady) return 999.0f;
  uint16_t valid[RANGE_SAMPLE_COUNT];
  int count = 0;
  Serial.println("Distance samples: mm / range status / API status");
  for (int i = 0; i < RANGE_SAMPLE_COUNT; ++i) {
    VL53L0X_RangingMeasurementData_t measurement = {};
    VL53L0X_Error error = lox.getSingleRangingMeasurement(&measurement, false);
    bool accepted = error == VL53L0X_ERROR_NONE &&
                    measurement.RangeStatus == 0 &&
                    measurement.RangeMilliMeter > RANGE_MIN_MM &&
                    measurement.RangeMilliMeter < RANGE_MAX_MM;
    Serial.printf("  %d: %u / %u / %d %s\n", i + 1,
                  (unsigned)measurement.RangeMilliMeter,
                  (unsigned)measurement.RangeStatus, (int)error,
                  accepted ? "OK" : "rejected");
    if (accepted) valid[count++] = measurement.RangeMilliMeter;
    delay(20);
  }
  if (count < RANGE_MIN_VALID_SAMPLES) {
    Serial.printf("Distance unavailable: only %d/%d valid samples.\n",
                  count, RANGE_SAMPLE_COUNT);
    return 999.0f; // Existing backend convention; never interpret as empty
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
  // Spread is diagnostic only: an acceptance threshold needs hopper testing.
  Serial.printf("Median: %.1f mm; valid: %d/%d; spread: %u mm\n",
                medianMM, count, RANGE_SAMPLE_COUNT,
                (unsigned)(valid[count - 1] - valid[0]));
  return medianMM / 10.0f; // Preserve existing Supabase distance units: cm
}

bool sendDataToSupabase(float distance, float battery, bool plugged) {
  if (String(SUPABASE_ANON_KEY).startsWith("PASTE_")) {
    Serial.println("Supabase skipped: enter your anon key in the sketch.");
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure(); // Migration compatibility: see security note above
  client.setHandshakeTimeout(15);
  HTTPClient http;
  http.setConnectTimeout(15000);
  http.setTimeout(15000);
  if (!http.begin(client, SUPABASE_URL)) {
    Serial.println("Supabase: failed to initialize HTTP.");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer", "return=minimal");
  String payload = String("{\"distance\":") + String(distance, 2)
    + ",\"battery_voltage\":" + String(battery, 2)
    + ",\"is_plugged\":" + (plugged ? "true" : "false")
    + ",\"firmware_version\":" + String(CURRENT_VERSION) + "}";
  int code = http.POST(payload);
  if (code >= 200 && code < 300) {
    Serial.printf("Supabase success: HTTP %d\n", code);
  } else if (code > 0) {
    Serial.printf("Supabase rejected insert: HTTP %d\n", code);
    Serial.println(http.getString());
  } else {
    Serial.printf("Supabase network error: %s\n", http.errorToString(code).c_str());
  }
  http.end();
  return code >= 200 && code < 300;
}

int fetchLatestVersion() {
  WiFiClientSecure client;
  client.setInsecure(); // Migration compatibility: see security note above
  client.setHandshakeTimeout(15);
  HTTPClient http;
  http.setConnectTimeout(15000);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, VERSION_URL)) return -1;
  int code = http.GET();
  String value;
  if (code == HTTP_CODE_OK) value = http.getString();
  http.end(); // Release HTTP connection before opening firmware download
  if (code != HTTP_CODE_OK) {
    Serial.printf("C3 version check failed: %d; continuing with readings.\n", code);
    return -1;
  }
  value.trim();
  if (value.isEmpty() || value.length() > 9) return -1;
  for (unsigned int i = 0; i < value.length(); ++i) {
    if (value[i] < '0' || value[i] > '9') return -1;
  }
  long version = value.toInt();
  return version > 0 && version <= INT_MAX ? (int)version : -1;
}

void checkForUpdates() {
  Serial.println("Checking ESP32-C3 firmware channel...");
  int latest = fetchLatestVersion();
  if (latest <= CURRENT_VERSION) {
    if (latest > 0) Serial.println("No newer C3 firmware.");
    return;
  }
  Serial.printf("Updating firmware %d -> %d\n", CURRENT_VERSION, latest);
  WiFiClientSecure client;
  client.setInsecure(); // Migration compatibility: see security note above
  client.setHandshakeTimeout(15);
  httpUpdate.rebootOnUpdate(false);
  t_httpUpdate_return result = httpUpdate.update(client, FIRMWARE_URL);
  if (result == HTTP_UPDATE_OK) {
    Serial.println("OTA succeeded; rebooting.");
    Serial.flush();
    ESP.restart();
  } else if (result == HTTP_UPDATE_FAILED) {
    Serial.printf("OTA failed (%d): %s\n", httpUpdate.getLastError(),
                  httpUpdate.getLastErrorString().c_str());
  } else {
    Serial.println("OTA server returned no update.");
  }
}

bool initializeSensor() {
  wakeSensor();
  sensorReady = lox.begin(0x29, false, &Wire);
  if (sensorReady) {
    sensorReady = lox.setMeasurementTimingBudgetMicroSeconds(RANGE_TIMING_BUDGET_US);
  }
  Serial.println(sensorReady ? "VL53L0X ready, 100 ms timing budget." :
                               "VL53L0X initialization failed; will retry.");
  return sensorReady;
}

uint32_t nextSampleAt = 0;
uint32_t lastWiFiRetryAt = 0;
uint32_t cycles = 0, uploadsOK = 0, uploadsFailed = 0, offlineSkipped = 0;
uint32_t invalidDistances = 0, missedSlots = 0;
bool previouslyConnected = false;

void setup() {
  prepareSensorShutdown();
  Serial.begin(115200);
  delay(1000);
  Serial.printf("\nAI Coffee ESP32-C3 firmware %d -- CONTINUOUS TEST\n", CURRENT_VERSION);
  Serial.printf("Reset reason: %d\n", (int)esp_reset_reason());
  if (LED_PIN >= 0) pinMode(LED_PIN, OUTPUT);
  setLed(false);
  pinMode(BATTERY_PIN, INPUT);
  pinMode(PLUGIN_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
  analogSetPinAttenuation(PLUGIN_PIN, ADC_11db);
  if (LED_PIN >= 0) ticker.attach(0.2, tick);

  WiFi.onEvent(onWiFiEvent);
  if (!WiFi.mode(WIFI_STA) || !applyWiFiPower("before connection")) {
    stopOnWiFiPowerError();
  }
  {
    WiFiManager wm;
    wm.setDebugOutput(false);
    wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT_SECONDS);
    wm.setConfigPortalTimeout(PORTAL_TIMEOUT_SECONDS);
    wm.setAPCallback(configModeCallback);
    wm.setSaveConfigCallback(credentialsSavedCallback);
    bool connected = FORCE_CONFIG_PORTAL
      ? wm.startConfigPortal("AI Coffee", "12345678")
      : wm.autoConnect("AI Coffee", "12345678");
    if (!connected || WiFi.status() != WL_CONNECTED) {
      Serial.println("Wi-Fi unavailable: continue measuring offline; retry saved credentials.");
    }
  }
  ticker.detach();
  setLed(false);
  if (!WiFi.mode(WIFI_STA) || !applyWiFiPower("after provisioning")) {
    stopOnWiFiPowerError();
  }
  WiFi.setAutoReconnect(true);
  previouslyConnected = WiFi.status() == WL_CONNECTED;
  if (previouslyConnected) {
    Serial.print("Connected; IP: ");
    Serial.println(WiFi.localIP());
    if (CHECK_OTA_AT_BOOT) checkForUpdates();
  }
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(100);
  initializeSensor();
  lastWiFiRetryAt = millis();
  nextSampleAt = millis();
  Serial.println("Test started: 2000 ms target, no deep sleep; stop manually.");
}

void loop() {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !previouslyConnected) {
    if (!applyWiFiPower("reconnected")) stopOnWiFiPowerError();
    Serial.print("Reconnected; IP: ");
    Serial.println(WiFi.localIP());
  }
  previouslyConnected = connected;
  if (!connected && (uint32_t)(millis() - lastWiFiRetryAt) >= WIFI_RETRY_INTERVAL_MS) {
    lastWiFiRetryAt = millis();
    Serial.println("Retrying saved Wi-Fi connection...");
    WiFi.reconnect();
  }
  if ((int32_t)(millis() - nextSampleAt) < 0) {
    delay(2);
    return;
  }
  uint32_t startedAt = millis();
  nextSampleAt += SAMPLE_INTERVAL_MS;
  ++cycles;
  Serial.printf("\n=== Cycle %lu | uptime %lu ms ===\n",
                (unsigned long)cycles, (unsigned long)startedAt);
  if (!sensorReady) initializeSensor();
  float distance = readDistance();
  if (distance == 999.0f) ++invalidDistances;
  shutDownSensor(); // No-op with current USE_XSHUT=false
  float battery = readBattery();
  bool plugged = readPluginStatus();
  Serial.printf("Distance: %.2f cm; battery: %.3f V; external power: %s\n",
                distance, battery, plugged ? "yes" : "no");
  uint32_t uploadStartedAt = millis();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi RSSI: %ld dBm\n", (long)WiFi.RSSI());
    if (sendDataToSupabase(distance, battery, plugged)) ++uploadsOK;
    else ++uploadsFailed;
  } else {
    ++offlineSkipped;
    Serial.println("Offline: measurement logged locally; upload skipped (not queued).");
  }
  uint32_t finishedAt = millis();
  uint32_t skippedNow = 0;
  if ((int32_t)(finishedAt - nextSampleAt) >= 0) {
    skippedNow = (finishedAt - nextSampleAt) / SAMPLE_INTERVAL_MS + 1;
    nextSampleAt += skippedNow * SAMPLE_INTERVAL_MS;
    missedSlots += skippedNow;
    Serial.printf("OVERRUN: skipped %lu scheduled slots; no catch-up uploads.\n",
                  (unsigned long)skippedNow);
  }
  Serial.printf("Cycle %lu ms | upload/skip %lu ms | heap %lu bytes (min %lu)\n",
                (unsigned long)(finishedAt - startedAt),
                (unsigned long)(finishedAt - uploadStartedAt),
                (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
  Serial.printf("Totals: cycles=%lu OK=%lu failed=%lu offline=%lu invalid_distance=%lu missed_slots=%lu\n",
                (unsigned long)cycles, (unsigned long)uploadsOK,
                (unsigned long)uploadsFailed, (unsigned long)offlineSkipped,
                (unsigned long)invalidDistances, (unsigned long)missedSlots);
}
