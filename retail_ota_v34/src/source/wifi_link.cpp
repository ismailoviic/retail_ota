#include "../include/wifi_link.h"

#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_wifi.h>

#include "../include/app_state.h"
#include "../include/awake_watchdog.h"
#include "../include/power_led.h"

uint32_t wifiConnectMs = 0;

static uint32_t wifiAttemptStartMs = 0;
static bool portalUsed = false;
static bool fastConnectUsed = false;
static volatile bool staAssociated = false;
static wifi_config_t savedStaConfig = {};
static bool staConfigPinned = false;

static bool wifiInBackoff() {
  return timerWake && retained.failedWifiWakes >= WIFI_BACKOFF_AFTER_FAILED_WAKES;
}

// --------------------------------------------------
// Wi-Fi power and callbacks
// --------------------------------------------------

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  (void)info;
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    staAssociated = true;
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    LOGF(
      "Wi-Fi disconnected; reason code: %u\n",
      (unsigned)info.wifi_sta_disconnected.reason);
  }
}

static bool applyWiFiPower(const char* stage) {
  bool ok = WiFi.setTxPower(WIFI_TX_POWER);

  LOGF(
    "Wi-Fi power [%s]: %s; reported limit %.2f dBm\n",
    stage,
    ok ? "accepted" : "FAILED",
    (int)WiFi.getTxPower() / 4.0f);

  return ok;
}

static void configModeCallback(WiFiManager*) {
  ledSolidOn();

  if (!applyWiFiPower("configuration portal")) {
    LOGLN("Portal TX power failed; sleeping.");
    goToDeepSleep();
    return;
  }

  LOGLN("Wi-Fi portal: AI Coffee / 12345678");
#if ENABLE_SERIAL_LOG
  Serial.print("Open http://");
  Serial.println(WiFi.softAPIP());
#endif
  LOGLN("Portal timeout: 180 seconds, then sleep.");
}

static void credentialsSavedCallback() {
  LOGLN(
    "Wi-Fi settings saved; continuing without an extra reboot.");
}

// --------------------------------------------------
// Cached-AP fast reconnect
// --------------------------------------------------

static void restoreStationConfig() {
  // Stored (unpinned) config back in memory before anything may write to NVS again.
  // If that fails, stay on RAM storage: a failed wake is better than persisting the pin.
  if (!staConfigPinned) return;
  if (esp_wifi_set_config(WIFI_IF_STA, &savedStaConfig) == ESP_OK) {
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);  // Arduino default (WiFi.persistent(true)).
  }
  staConfigPinned = false;
}

static bool startCachedApConnect() {
  if (!retained.apChannel) return false;
  if (esp_wifi_get_config(WIFI_IF_STA, &savedStaConfig) != ESP_OK || !savedStaConfig.sta.ssid[0]) return false;

  // Stored fields are fixed-size and not always NUL-terminated.
  char ssid[33] = {};
  char pass[65] = {};
  memcpy(ssid, savedStaConfig.sta.ssid, sizeof(savedStaConfig.sta.ssid));
  memcpy(pass, savedStaConfig.sta.password, sizeof(savedStaConfig.sta.password));

  // RAM storage for the rest of this wake: the pinned BSSID/channel, and the core's own
  // reconnect that re-applies the current config, must never reach the credentials in NVS.
  if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) return false;
  staConfigPinned = true;
  if (WiFi.begin(ssid, pass[0] ? pass : nullptr, retained.apChannel, retained.apBssid) == WL_CONNECT_FAILED) {
    restoreStationConfig();
    return false;
  }
  return true;
}

static void rememberConnectedAp() {
  uint8_t bssid[6] = {};
  int32_t channel = WiFi.channel();
  bool bssidValid = WiFi.BSSID(bssid) && (bssid[0] | bssid[1] | bssid[2] | bssid[3] | bssid[4] | bssid[5]);
  if (!bssidValid || channel <= 0 || channel > 14) {
    retained.apChannel = 0;
    return;
  }
  memcpy(retained.apBssid, bssid, sizeof(retained.apBssid));
  retained.apChannel = (uint8_t)channel;
}

static void startFullScanAttempt() {
  LOGLN("Wi-Fi attempt: stored credentials, full scan");
  staAssociated = false;
  WiFi.begin();  // Stored credentials; never erase them on a transient failure.
  wifiAttemptStartMs = millis();
}

static bool waitForWiFi(uint32_t associateTimeoutMs, uint32_t timeoutMs) {
  while (WiFi.status() != WL_CONNECTED) {
    uint32_t waited = millis() - wifiAttemptStartMs;
    if (waited >= timeoutMs || (!staAssociated && waited >= associateTimeoutMs)) break;
    delay(20);
  }
  return WiFi.status() == WL_CONNECTED;
}

// --------------------------------------------------
// Connection
// --------------------------------------------------

bool startWiFi() {
  networkStartMs = millis();
  WiFi.onEvent(onWiFiEvent);
  WiFi.setAutoReconnect(false);
  if (!WiFi.mode(WIFI_STA) || !applyWiFiPower("before connection")) {
    ++retained.wifiFailures;
    return false;
  }
  if (!retained.session[0]) makeUuid(retained.session);
  if (FORCE_CONFIG_PORTAL) return true;

  staAssociated = false;
  fastConnectUsed = startCachedApConnect();
  if (fastConnectUsed) {
    LOGF("Wi-Fi attempt: cached AP on channel %u\n", (unsigned)retained.apChannel);
    wifiAttemptStartMs = millis();
  } else {
    startFullScanAttempt();
  }
  return true;
}

bool finishWiFi() {
  bool connected = false;
  if (!FORCE_CONFIG_PORTAL) {
    const uint32_t timeoutMs = WIFI_CONNECT_TIMEOUT_SECONDS * 1000UL;
    int fullAttemptsLeft = wifiInBackoff() ? 1 : 2;
    bool fastAttempt = fastConnectUsed;
    while (true) {
      if (waitForWiFi(fastAttempt ? FAST_ASSOCIATE_TIMEOUT_MS : timeoutMs, timeoutMs)) {
        connected = true;
        break;
      }
      WiFi.disconnect(false, false);
      if (fastAttempt) {
        // AP moved or changed channel: forget it and fall back to the normal scan.
        LOGLN("Cached AP not reached.");
        restoreStationConfig();
        retained.apChannel = 0;
        fastAttempt = false;
      } else {
        ++retained.wifiFailures;
        if (--fullAttemptsLeft <= 0) break;
      }
      delay(250);
      startFullScanAttempt();
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
    setWakeStage(STAGE_PORTAL);
    extendAwakeDeadline(PORTAL_DEADLINE_MS);
    connected = wm.startConfigPortal("AI Coffee", "12345678");
    if (!connected) ++retained.wifiFailures;
    extendAwakeDeadline(ONLINE_DEADLINE_MS);  // Publishing starts over after the portal
    setWakeStage(STAGE_WIFI);
  }
  wifiConnectMs = millis() - networkStartMs;
  stopLedBlink();
  if (!connected || WiFi.status() != WL_CONNECTED) return false;
  // Verify only; the limit set before connecting normally persists.
  if (WiFi.getTxPower() > WIFI_TX_POWER && !applyWiFiPower("connected")) {
    ++retained.wifiFailures;
    return false;
  }
  rememberConnectedAp();
  if (portalUsed) networkStartMs = millis();  // Portal is an explicit scheduling exception.
  LOGF("Connected: %s; RSSI %ld dBm; connect %lu ms\n",
       WiFi.localIP().toString().c_str(), (long)WiFi.RSSI(), (unsigned long)wifiConnectMs);
  return true;
}
