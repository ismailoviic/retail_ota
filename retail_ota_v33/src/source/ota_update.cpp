#include "../include/ota_update.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>

#include "../include/app_state.h"
#include "../include/power_led.h"

static void printOTAPartitions() {
#if ENABLE_SERIAL_LOG
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
#endif
}

static int fetchLatestVersion() {
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(4);

  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  // No cache-buster: with a daily check, GitHub's ~5 min CDN cache is acceptable.
  LOGF("[OTA] Version URL: %s\n", VERSION_URL);

  if (!http.begin(client, VERSION_URL)) {
    LOGLN("[OTA] HTTP initialization failed.");
    return -1;
  }

  int code = http.GET();
  LOGF("[OTA] Version HTTP status=%d\n", code);

  if (code != HTTP_CODE_OK) {
    if (code < 0) {
      LOGLN(http.errorToString(code));
    }

    if (code == 404) {
      LOGLN(
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
    LOGLN("[OTA] Invalid version.txt: use an integer.");
    return -1;
  }

  for (unsigned int i = 0; i < value.length(); ++i) {
    if (value[i] < '0' || value[i] > '9') {
      LOGLN("[OTA] Use an integer, not v26, JSON, or HTML.");
      return -1;
    }
  }

  int latest = value.toInt();

  if (latest <= 0) {
    return -1;
  }

  LOGF(
    "[OTA] Installed=%d; published=%d\n",
    CURRENT_VERSION,
    latest);

  return latest;
}

static void onOTAProgress(int current, int total) {
  static int lastBucket = -1;

  int percent = total > 0
                  ? (int)((int64_t)current * 100 / total)
                  : 0;

  int bucket = percent / 10;

  if (bucket != lastBucket || current == total) {
    LOGF(
      "[OTA] Download: %d/%d bytes (%d%%)\n",
      current,
      total,
      percent);

    lastBucket = bucket;
  }
}

static void checkForUpdates() {
  if (WiFi.status() != WL_CONNECTED) {
    LOGLN("[OTA] Skipped: Wi-Fi disconnected.");
    return;
  }

  printOTAPartitions();
  startLedBlink(0.10f);

  int latest = fetchLatestVersion();

  if (latest <= CURRENT_VERSION) {
    LOGLN(
      latest > 0
        ? "[OTA] No newer firmware."
        : "[OTA] Check failed; retry at the next daily check.");

    stopLedBlink();
    return;
  }

  const esp_partition_t* target =
    esp_ota_get_next_update_partition(nullptr);

  if (!target) {
    LOGLN(
      "[OTA] No OTA slot. USB flash with an OTA-capable layout.");

    stopLedBlink();
    return;
  }

  LOGF(
    "[OTA] Updating %d -> %d\n",
    CURRENT_VERSION,
    latest);
  LOGF("[OTA] Binary URL: %s\n", FIRMWARE_URL);

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(4);

  // The version parameter is enough to bypass a stale CDN copy of the previous binary.
  String binaryURL =
    String(FIRMWARE_URL)
    + "?version=" + String(latest);

  httpUpdate.rebootOnUpdate(false);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.onProgress(onOTAProgress);

  t_httpUpdate_return result =
    httpUpdate.update(client, binaryURL);

  stopLedBlink();

  if (result == HTTP_UPDATE_OK) {
    LOGLN("[OTA] Install succeeded; rebooting.");
#if ENABLE_SERIAL_LOG
    Serial.flush();
#endif
    ESP.restart();

  } else if (result == HTTP_UPDATE_FAILED) {
    LOGF(
      "[OTA] Failed (%d): %s\n",
      httpUpdate.getLastError(),
      httpUpdate.getLastErrorString().c_str());

    flashUploadError();

  } else {
    LOGLN("[OTA] Server returned no update.");
  }
}

void checkForUpdatesIfDue() {
  uint64_t elapsed = elapsedNowMs();
  if (elapsed >= retained.nextOtaMs && networkBudgetAvailable(12000)) {
    retained.nextOtaMs = elapsed + OTA_CHECK_INTERVAL_MS;
    checkForUpdates();  // May exceed reporting interval when downloading firmware.
  }
}
