#include "../include/app_state.h"

#include <ctype.h>
#include <Preferences.h>
#include <esp_random.h>
#include <esp_rom_crc.h>
#include <esp_system.h>

#include "../include/awake_watchdog.h"

RTC_DATA_ATTR RetainedState retained;
static constexpr uint32_t RTC_MAGIC = 0xC0FF0034;

// Small copy of the schedule state in memory the boot code never clears, so a watchdog or
// software reset (and usually the RESET pin) resumes the refill check instead of losing it.
// A power cut leaves garbage, rejected by the magic, version and CRC.
struct ResetSurvivingState {
  uint32_t magic;
  int32_t version;
  uint8_t mode;
  uint8_t modeBeforeConfirm;
  uint8_t confirmReadings;
  uint8_t confirmValid;
  int16_t preJumpTenths;
  uint8_t retryNightCheck;
  uint8_t fillCount;
  uint8_t fillHead;
  int16_t fillTenths[FILL_HISTORY];
  uint32_t watchdogTrips;
  uint32_t crc;  // Over everything above
};
RTC_NOINIT_ATTR static ResetSurvivingState survivor;
static constexpr uint32_t SURVIVOR_MAGIC = 0x5AFE0034;

static uint32_t survivorCrc() {
  return esp_rom_crc32_le(0, (const uint8_t*)&survivor, offsetof(ResetSurvivingState, crc));
}

void saveResetSurvivingState() {
  memset(&survivor, 0, sizeof(survivor));  // Defined padding for the CRC
  survivor.magic = SURVIVOR_MAGIC;
  survivor.version = CURRENT_VERSION;
  survivor.mode = retained.mode;
  survivor.modeBeforeConfirm = retained.modeBeforeConfirm;
  survivor.confirmReadings = retained.confirmReadings;
  survivor.confirmValid = retained.confirmValid;
  survivor.preJumpTenths = retained.preJumpTenths;
  survivor.retryNightCheck = retained.retryNightCheck;
  survivor.fillCount = retained.fillCount;
  survivor.fillHead = retained.fillHead;
  memcpy(survivor.fillTenths, retained.fillTenths, sizeof(survivor.fillTenths));
  survivor.watchdogTrips = retained.watchdogTrips;
  survivor.crc = survivorCrc();
}

static void restoreResetSurvivingState() {
  if (survivor.magic != SURVIVOR_MAGIC || survivor.version != CURRENT_VERSION
      || survivor.crc != survivorCrc() || survivor.mode > MODE_REFILL_CONFIRM
      || survivor.fillCount > FILL_HISTORY || survivor.fillHead >= FILL_HISTORY) {
    return;
  }
  retained.mode = survivor.mode;
  retained.modeBeforeConfirm = survivor.modeBeforeConfirm;
  retained.confirmReadings = survivor.confirmReadings;
  retained.confirmValid = survivor.confirmValid;
  retained.preJumpTenths = survivor.preJumpTenths;
  retained.retryNightCheck = survivor.retryNightCheck;
  retained.fillCount = survivor.fillCount;
  retained.fillHead = survivor.fillHead;
  memcpy(retained.fillTenths, survivor.fillTenths, sizeof(retained.fillTenths));
  retained.watchdogTrips = survivor.watchdogTrips;
  retained.stateRestored = 1;
}

uint32_t cycleStartMs = 0;
uint32_t networkStartMs = 0;
uint32_t sampleTakenMs = 0;
bool timerWake = false;
uint32_t selectedInterval = NORMAL_INTERVAL;
uint32_t sleepInterval = NORMAL_INTERVAL;
char deviceId[37] = {};

void initRetainedState() {
  if (esp_reset_reason() != ESP_RST_DEEPSLEEP || retained.magic != RTC_MAGIC) {
    memset(&retained, 0, sizeof(retained));
    retained.magic = RTC_MAGIC;
    restoreResetSurvivingState();
    uint8_t stage = hangStageBeforeReset();  // RTC watchdog (or IDF watchdog) reset
    if (stage != STAGE_NONE) {
      ++retained.watchdogTrips;
      retained.pendingHangStage = stage;
    }
  }
}

bool radioOffMode() {
  return retained.mode == MODE_OFFLINE || retained.mode == MODE_REFILL_CONFIRM;
}

static bool validUuid(const char* value) {
  if (!value || strlen(value) != 36) return false;
  for (int i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') return false;
    } else if (!isxdigit((unsigned char)value[i])) return false;
  }
  return true;
}

// Stored value wins; the compile-time initial value is written only on first provisioning.
// The RTC copy (output) is reused on timer wakes, so NVS is read only after power-on/RESET.
static bool loadProvisionedValue(const char* key, const char* initial, char* output, size_t outputSize,
                                 bool (*valid)(const char*)) {
  if (valid(output)) return true;

  char stored[65] = {};
  bool hasKey = false;
  Preferences prefs;
  // Read-only open fails when the namespace does not exist yet (first provisioning).
  if (prefs.begin("coffee-id", true)) {
    hasKey = prefs.isKey(key);
    if (hasKey) prefs.getString(key, stored, sizeof(stored));
    prefs.end();
  }
  if (!hasKey) {
    if (!valid(initial) || !prefs.begin("coffee-id", false)) return false;
    bool saved = prefs.putString(key, initial) > 0;
    prefs.end();
    if (!saved) return false;
    strlcpy(stored, initial, sizeof(stored));
  }
  if (!valid(stored) || strlen(stored) >= outputSize) return false;
  strlcpy(output, stored, outputSize);
  return true;
}

bool loadDeviceIdentity() {
  if (!loadProvisionedValue("device_id", INITIAL_DEVICE_ID, retained.deviceId,
                            sizeof(retained.deviceId), validUuid)) {
    return false;
  }
  memcpy(deviceId, retained.deviceId, sizeof(deviceId));
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

// Approximate time since the RTC state was created; drifts with the RC slow clock.
uint64_t elapsedNowMs() {
  return retained.elapsedMs + (millis() - cycleStartMs);
}

bool networkBudgetAvailable(uint32_t reserveMs) {
  return (uint32_t)(millis() - networkStartMs) + reserveMs < NETWORK_WORK_BUDGET_MS;
}
