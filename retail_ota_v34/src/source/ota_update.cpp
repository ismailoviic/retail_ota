#include "../include/ota_update.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClient.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

#include "../include/app_state.h"
#include "../include/awake_watchdog.h"
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
  WiFiClient client;  // Plain HTTP: TLS alone cost ~95 KB of flash, and v33 skipped its checks.

  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(4000);

  // nginx serves the file with Cache-Control: no-cache.
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
        "[OTA] Check version.txt in the server's coffee-counter folder.");
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

static int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// The published SHA-256 of the binary: 64 hex characters (sha256sum output accepted).
static bool fetchExpectedSha256(uint8_t digest[32]) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  if (!http.begin(client, FIRMWARE_SHA256_URL)) return false;
  int code = http.GET();
  String value = code == HTTP_CODE_OK ? http.getString() : String();
  http.end();
  value.trim();
  if (value.length() < 64) {
    LOGF("[OTA] No SHA-256 (HTTP %d); no install.\n", code);
    return false;
  }
  for (int i = 0; i < 32; ++i) {
    int high = hexValue(value[2 * i]), low = hexValue(value[2 * i + 1]);
    if (high < 0 || low < 0) {
      LOGLN("[OTA] Invalid SHA-256 file; no install.");
      return false;
    }
    digest[i] = (uint8_t)(high << 4 | low);
  }
  return true;
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

// Streams the binary into the spare slot while hashing it. The slot is marked bootable only
// when the whole file arrived and its SHA-256 matches the published one.
static bool downloadAndVerify(const String& url, const esp_partition_t* target, const uint8_t expected[32]) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  int total = http.getSize();
  if (code != HTTP_CODE_OK || total <= 0 || (uint32_t)total > target->size) {
    LOGF("[OTA] Binary HTTP %d, %d bytes; no install.\n", code, total);
    http.end();
    return false;
  }
  if (!Update.begin(total, U_FLASH)) {
    LOGF("[OTA] Update.begin failed: %s\n", Update.errorString());
    http.end();
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  static uint8_t chunk[1024];  // Static, not on the setup() stack.
  NetworkClient* stream = http.getStreamPtr();
  int remaining = total;
  uint32_t lastDataMs = millis();
  bool writeFailed = false;
  while (remaining > 0) {
    size_t available = stream->available();
    if (!available) {
      if (!stream->connected() || millis() - lastDataMs >= OTA_STALL_MS) break;
      delay(5);
      continue;
    }
    int n = stream->readBytes(chunk, min(available, min(sizeof(chunk), (size_t)remaining)));
    if (n <= 0) continue;
    mbedtls_sha256_update(&sha, chunk, n);
    if (Update.write(chunk, n) != (size_t)n) {
      writeFailed = true;
      break;
    }
    remaining -= n;
    lastDataMs = millis();
    extendAwakeDeadline(OTA_CHUNK_DEADLINE_MS);  // Slow but progressing: keep going
    onOTAProgress(total - remaining, total);
  }
  http.end();

  uint8_t actual[32];
  mbedtls_sha256_finish(&sha, actual);
  mbedtls_sha256_free(&sha);
  if (writeFailed || remaining > 0) {
    LOGF("[OTA] Download incomplete (%d bytes missing): %s\n", remaining, Update.errorString());
    Update.abort();
    return false;
  }
  if (memcmp(actual, expected, sizeof(actual)) != 0) {
    LOGLN("[OTA] SHA-256 mismatch; download discarded.");
    Update.abort();
    return false;
  }
  // end() also checks the image's own appended hash, then selects the slot for the next boot.
  if (!Update.end()) {
    LOGF("[OTA] Image rejected: %s\n", Update.errorString());
    return false;
  }
  return true;
}

static void checkForUpdates() {
  if (WiFi.status() != WL_CONNECTED) {
    LOGLN("[OTA] Skipped: Wi-Fi disconnected.");
    return;
  }

  printOTAPartitions();
  startLedBlink(0.10f);
  extendAwakeDeadline(ONLINE_DEADLINE_MS);  // Version, SHA-256 and binary requests; then per chunk

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

  uint8_t expected[32];
  if (!fetchExpectedSha256(expected)) {
    stopLedBlink();
    return;
  }

  // The version parameter bypasses any cache holding the previous binary.
  String binaryURL =
    String(FIRMWARE_URL)
    + "?version=" + String(latest);

  if (downloadAndVerify(binaryURL, target, expected)) {
    stopLedBlink();
    LOGLN("[OTA] Install succeeded; rebooting.");
#if ENABLE_SERIAL_LOG
    Serial.flush();
#endif
    ESP.restart();
  }

  stopLedBlink();
  flashUploadError();
}

void checkForUpdatesIfDue() {
  uint64_t elapsed = elapsedNowMs();
  if (elapsed >= retained.nextOtaMs && networkBudgetAvailable(12000)) {
    retained.nextOtaMs = elapsed + OTA_CHECK_INTERVAL_MS;
    checkForUpdates();  // May exceed reporting interval when downloading firmware.
  }
}
