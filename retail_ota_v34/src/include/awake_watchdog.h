#pragma once

// Two-layer watchdog for a stuck wake (27 Oct: frozen on the charger until a manual reset).
// 1. Awake deadline (esp_timer): past it, the wake is abandoned and the chip goes straight to
//    deep sleep; counters and session are kept.
// 2. RTC hardware watchdog: full chip reset if even the deadline cannot run.
// The stage in progress is kept in memory that survives the reset, and published once.

#include <stdint.h>

enum WakeStage : uint8_t {
  STAGE_NONE = 0,
  STAGE_BOOT,  // Before the first named stage
  STAGE_SENSOR,
  STAGE_WIFI,
  STAGE_PORTAL,
  STAGE_NTP,
  STAGE_MQTT,
  STAGE_OTA,
  STAGE_COUNT,
};

// Arms both layers; deadlineMs from now.
void startWatchdogs(uint32_t deadlineMs);
// Moves the deadline to deadlineMs from now (Wi-Fi started late, portal, OTA progress).
void extendAwakeDeadline(uint32_t deadlineMs);
void setWakeStage(WakeStage stage);
// Before a normal deep sleep: deadline cancelled, RTC watchdog off (it counts in deep sleep).
void stopWatchdogs();
// Stage recorded when the previous run ended in a watchdog reset, else STAGE_NONE.
uint8_t hangStageBeforeReset();
const char* wakeStageName(uint8_t stage);  // null for STAGE_NONE
