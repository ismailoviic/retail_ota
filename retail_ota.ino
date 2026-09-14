/*
  AI Coffee — ESP32-C3 — Firmware 25

  Normal operation:
  1. Wake and initialize.
  2. Read distance, battery voltage, and external-power status.
  3. Connect using saved Wi-Fi credentials.
     If necessary, open configuration portal for up to 180 seconds.
  4. Upload one record to Supabase.
  5. Check GitHub for an OTA update.
  6. Deep sleep for 600 seconds.

  The interval between uploads includes awake time + 600 seconds asleep.
  Failed Wi-Fi connection: sleep and retry on the next wake.
  Failed POST: log failure, check OTA if connected, then sleep.
  No queue or automatic retry of the same POST.

  Current wiring:
    Battery divider midpoint -> GPIO0
    USB-input divider midpoint -> GPIO1
    VL53L0X SDA -> GPIO4
    VL53L0X SCL -> GPIO5
    Built-in LED -> GPIO8, active LOW
    Optional XSHUT -> GPIO6, disabled by default

  Both dividers remain 10k top / 10k bottom, multiplier 2.
  Distance units: cm. 999 means unavailable, not empty.
  is_plugged means external power present, not charging current.

  Existing repository layout:
    main/version.txt
    main/build/esp32.esp32.esp32c3/retail_ota.ino.bin

  TLS setInsecure() is retained from v23.
  Server authentication and the charging-time battery measurement
  discrepancy are not resolved by this firmware.

  Source-reviewed; not compiled or hardware-tested here.
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
#include <esp_ota_ops.h>
#include <driver/gpio.h>

#if !defined(CONFIG_IDF_TARGET_ESP32C3)
#error "Select an ESP32-C3 board."
#endif

// --------------------------------------------------
// Firmware and timing
// --------------------------------------------------

constexpr int CURRENT_VERSION = 25;
constexpr uint64_t SLEEP_SECONDS = 600;
constexpr uint64_t US_PER_SECOND = 1000000ULL;

constexpr unsigned long WIFI_CONNECT_TIMEOUT_SECONDS = 10;
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

void goToDeepSleep();

// --------------------------------------------------
// LED
// --------------------------------------------------

void setLed(bool on) {
  if (LED_PIN < 0) return;

  digitalWrite(
    LED_PIN,
    (on != LED_ACTIVE_LOW) ? HIGH : LOW
  );
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
        esp_err_to_name(err)
      );
    }
  }

  if (USE_XSHUT) {
    esp_err_t err = gpio_hold_en((gpio_num_t)XSHUT_PIN);
    if (err != ESP_OK) {
      Serial.printf(
        "XSHUT hold failed: %s\n",
        esp_err_to_name(err)
      );
    }
  }

  gpio_deep_sleep_hold_en();

  Serial.printf(
    "Awake time: %lu ms. Sleeping for %llu seconds.\n",
    (unsigned long)millis(),
    (unsigned long long)SLEEP_SECONDS
  );

  esp_sleep_enable_timer_wakeup(
    SLEEP_SECONDS * US_PER_SECOND
  );

  Serial.flush();
  esp_deep_sleep_start();
}

// --------------------------------------------------
// Wi-Fi power and callbacks
// --------------------------------------------------

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_START ||
      event == ARDUINO_EVENT_WIFI_AP_START) {

    esp_err_t err =
      esp_wifi_set_max_tx_power(WIFI_TX_POWER_QUARTER_DBM);

    Serial.printf(
      "Wi-Fi %s start: 8.5 dBm request %s\n",
      event == ARDUINO_EVENT_WIFI_AP_START ? "AP" : "STA",
      esp_err_to_name(err)
    );

  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf(
      "Wi-Fi disconnected; reason code: %u\n",
      (unsigned)info.wifi_sta_disconnected.reason
    );
  }
}

bool applyWiFiPower(const char* stage) {
  bool ok = WiFi.setTxPower(WIFI_TX_POWER);

  Serial.printf(
    "Wi-Fi power [%s]: %s; reported limit %.2f dBm\n",
    stage,
    ok ? "accepted" : "FAILED",
    (int)WiFi.getTxPower() / 4.0f
  );

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
    "Wi-Fi settings saved; continuing without an extra reboot."
  );
}

bool connectWiFi() {
  startLedBlink(0.2f);

  WiFi.onEvent(onWiFiEvent);

  if (!WiFi.mode(WIFI_STA) ||
      !applyWiFiPower("before connection")) {
    stopLedBlink();
    return false;
  }

  bool connected = false;

  {
    WiFiManager wm;
    wm.setDebugOutput(false);
    wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT_SECONDS);
    wm.setConfigPortalTimeout(PORTAL_TIMEOUT_SECONDS);
    wm.setAPCallback(configModeCallback);
    wm.setSaveConfigCallback(credentialsSavedCallback);

    connected = FORCE_CONFIG_PORTAL
      ? wm.startConfigPortal("AI Coffee", "12345678")
      : wm.autoConnect("AI Coffee", "12345678");
  }

  // Portal objects are released before opening TLS connections.
  stopLedBlink();

  if (!connected || WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi unavailable / portal timed out.");
    return false;
  }

  if (!applyWiFiPower("connected")) {
    return false;
  }

  Serial.print("Connected; IP: ");
  Serial.println(WiFi.localIP());

  Serial.printf(
    "Signal: %ld dBm\n",
    (long)WiFi.RSSI()
  );

  return true;
}

// --------------------------------------------------
// ADC measurements — v23 method
// --------------------------------------------------

float median3(float a, float b, float c) {
  if (a > b) {
    float t = a; a = b; b = t;
  }
  if (b > c) {
    float t = b; b = c; c = t;
  }
  if (a > b) {
    float t = a; a = b; b = t;
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
    * BATTERY_GAIN + BATTERY_OFFSET_V;

  Serial.printf(
    "Battery ADC: %.3f V; reported battery: %.3f V\n",
    pinV,
    batteryV
  );

  return batteryV;
}

bool readPluginStatus() {
  float pinV = readPinVoltage(PLUGIN_PIN);

  float inputV =
    pinV * (1.0f + USB_R_TOP / USB_R_BOTTOM);

  Serial.printf(
    "USB ADC: %.3f V; estimated charger input: %.3f V\n",
    pinV,
    inputV
  );

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
        RANGE_TIMING_BUDGET_US
      );
  }

  Serial.println(
    sensorReady
      ? "VL53L0X ready, 100 ms timing budget."
      : "VL53L0X initialization failed; distance will be 999."
  );

  return sensorReady;
}

float readDistance() {
  if (!sensorReady) {
    return DISTANCE_UNAVAILABLE_CM;
  }

  uint16_t valid[RANGE_SAMPLE_COUNT];
  int count = 0;

  Serial.println(
    "Distance samples: mm / range status / API status"
  );

  for (int i = 0; i < RANGE_SAMPLE_COUNT; ++i) {
    VL53L0X_RangingMeasurementData_t measurement = {};

    VL53L0X_Error error =
      lox.getSingleRangingMeasurement(&measurement, false);

    bool accepted =
      error == VL53L0X_ERROR_NONE &&
      measurement.RangeStatus == 0 &&
      measurement.RangeMilliMeter > RANGE_MIN_MM &&
      measurement.RangeMilliMeter < RANGE_MAX_MM;

    Serial.printf(
      "  %d: %u / %u / %d %s\n",
      i + 1,
      (unsigned)measurement.RangeMilliMeter,
      (unsigned)measurement.RangeStatus,
      (int)error,
      accepted ? "OK" : "rejected"
    );

    if (accepted) {
      valid[count++] = measurement.RangeMilliMeter;
    }

    delay(20);
  }

  if (count < RANGE_MIN_VALID_SAMPLES) {
    Serial.printf(
      "Distance unavailable: only %d/%d valid samples.\n",
      count,
      RANGE_SAMPLE_COUNT
    );

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
    (unsigned)(valid[count - 1] - valid[0])
  );

  return medianMM / 10.0f;
}

// --------------------------------------------------
// Supabase
// --------------------------------------------------

bool sendDataToSupabase(
  float distance,
  float battery,
  bool plugged
) {
  if (String(SUPABASE_ANON_KEY).startsWith("PASTE_")) {
    Serial.println("Supabase skipped: enter your existing anon key.");
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Supabase skipped: Wi-Fi disconnected.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
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
  http.addHeader(
    "Authorization",
    String("Bearer ") + SUPABASE_ANON_KEY
  );
  http.addHeader("Prefer", "return=minimal");

  String payload =
    String("{\"distance\":") + String(distance, 2)
    + ",\"battery_voltage\":" + String(battery, 2)
    + ",\"is_plugged\":" + (plugged ? "true" : "false")
    + ",\"firmware_version\":" + String(CURRENT_VERSION)
    + "}";

  int code = http.POST(payload);

  if (code >= 200 && code < 300) {
    Serial.printf("Supabase success: HTTP %d\n", code);
  } else if (code > 0) {
    Serial.printf("Supabase rejected insert: HTTP %d\n", code);
    Serial.println(http.getString());
  } else {
    Serial.printf(
      "Supabase network error: %s\n",
      http.errorToString(code).c_str()
    );
  }

  http.end();

  return code >= 200 && code < 300;
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
    (unsigned long)ESP.getSketchSize()
  );

  if (running) {
    Serial.printf(
      "[OTA] Running partition=%s size=%lu\n",
      running->label,
      (unsigned long)running->size
    );
  }

  if (target) {
    Serial.printf(
      "[OTA] Next partition=%s size=%lu\n",
      target->label,
      (unsigned long)target->size
    );
  } else {
    Serial.println("[OTA] No update partition available.");
  }
}

int fetchLatestVersion() {
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(15);

  HTTPClient http;
  http.setConnectTimeout(15000);
  http.setTimeout(15000);
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
        "[OTA] Check root version.txt on the main branch."
      );
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
      Serial.println("[OTA] Use 25, not v25, JSON, or HTML.");
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
    latest
  );

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
      percent
    );

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
        : "[OTA] Check failed; retry next wake."
    );

    stopLedBlink();
    return;
  }

  const esp_partition_t* target =
    esp_ota_get_next_update_partition(nullptr);

  if (!target) {
    Serial.println(
      "[OTA] No OTA slot. USB flash with an OTA-capable layout."
    );

    stopLedBlink();
    return;
  }

  Serial.printf(
    "[OTA] Updating %d -> %d\n",
    CURRENT_VERSION,
    latest
  );
  Serial.printf("[OTA] Binary URL: %s\n", FIRMWARE_URL);

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(15);

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
      httpUpdate.getLastErrorString().c_str()
    );

    flashUploadError();

  } else {
    Serial.println("[OTA] Server returned no update.");
  }
}

// --------------------------------------------------
// Main cycle — version 17 order, C3/v23 adaptations
// --------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1000); // Bounded delay; no wait for a connected computer.

  initializeLed();
  prepareSensorShutdown();

  Serial.printf(
    "\nAI Coffee ESP32-C3 firmware %d — NORMAL MODE\n",
    CURRENT_VERSION
  );

  Serial.printf(
    "Reset reason: %d; wakeup cause: %d\n",
    (int)esp_reset_reason(),
    (int)esp_sleep_get_wakeup_cause()
  );

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

  // 1. Read before starting Wi-Fi, as in version 17.
  float distance = readDistance();
  shutDownSensor();

  float battery = readBattery();
  bool plugged = readPluginStatus();

  Serial.printf(
    "Distance: %.2f cm; battery: %.3f V; external power: %s\n",
    distance,
    battery,
    plugged ? "yes" : "no"
  );

  // 2. Connect or open the bounded configuration portal.
  if (!connectWiFi()) {
    Serial.println("Connection failed; retry after 10-minute sleep.");
    goToDeepSleep();
    return;
  }

  // 3. One upload attempt per wake.
  startLedBlink(0.10f);

  bool sent = sendDataToSupabase(distance, battery, plugged);

  stopLedBlink();

  if (!sent) {
    flashUploadError();
  }

  // 4. Check for an update after the upload, as in version 17.
  checkForUpdates();

  // 5. Sleep even if the POST or update check failed.
  goToDeepSleep();
}

void loop() {
  // Defensive fallback; normal execution sleeps at the end of setup().
  goToDeepSleep();
}