#pragma once

// State shared by the modules: what survives deep sleep, and this wake's cycle values.

#include "app_config.h"

// Wake schedule mode, kept across deep sleep. Radio-off modes start Wi-Fi only to publish.
enum ScheduleMode : uint8_t {
  MODE_ONLINE = 0,  // 30 min readings, published (also after a refill hold)
  MODE_OFFLINE,     // <= 15 %: 1 min readings into the track, radio off
  MODE_REFILL_CONFIRM,  // After a jump: 1 min readings, radio off, until confirmed or rejected
};

// POD only: retained across deep sleep; invalidated on any other reset or firmware change.
struct RetainedState {
  uint32_t magic;
  char session[37];
  char deviceId[37];
  uint32_t sequence;
  uint32_t wakes;
  uint32_t wifiFailures;
  uint32_t uploadFailures;
  uint32_t failedWifiWakes;
  uint32_t previousAwakeMs;
  int32_t previousPublishResult;  // 0 = no attempt, 1 = acknowledged, <0 = lwmqtt error
  uint32_t sensorFailWakes;
  // Last valid fills (x10) for refill detection and confirmation; fillHead is the next slot.
  uint8_t fillCount;
  uint8_t fillHead;
  int16_t fillTenths[FILL_HISTORY];
  // Schedule mode and the refill check in progress.
  uint8_t mode;               // ScheduleMode
  uint8_t modeBeforeConfirm;  // Restored when a jump is rejected
  uint8_t confirmReadings;    // Readings since the jump, including it
  uint8_t confirmValid;       // Consecutive valid readings since the jump
  int16_t preJumpTenths;      // Fill x10 before the jump
  uint8_t retryNightCheck;    // NTP failed after a refill: the next online wake retries
  // Offline track: oldest-first ring of fill x10, -1 = sensor failure.
  int16_t trackTenths[TRACK_CAPACITY];
  uint16_t trackHead;  // Next slot
  uint16_t trackCount;
  uint16_t trackFullReadings;  // Readings appended while full (upload retry pacing)
  uint32_t trackLastSequence;
  uint64_t trackLastElapsedMs;
  uint32_t tracksDropped;
  // Watchdog diagnostics.
  uint32_t watchdogTrips;
  uint8_t pendingHangStage;  // WakeStage of the last trip, published once
  uint8_t stateRestored;     // This session started from the reset-surviving copy
  uint64_t elapsedMs;
  uint64_t nextOtaMs;
  uint8_t apChannel;  // 0 = no cached AP
  uint8_t apBssid[6];
};
extern RetainedState retained;

// This wake.
extern uint32_t cycleStartMs;
extern uint32_t networkStartMs;
extern uint32_t sampleTakenMs;
extern bool timerWake;
extern uint32_t selectedInterval;  // From the wake schedule; published as interval_seconds.
extern uint32_t sleepInterval;     // Used by goToDeepSleep; Wi-Fi backoff may lengthen it.
extern char deviceId[37];

// Keeps the RTC state only across deep sleep with this firmware's layout. After any other
// reset, restores the schedule mode and last fills from the reset-surviving copy if valid.
void initRetainedState();
// Saves that copy (mode, refill check, last fills, watchdog trips); called before deep sleep.
void saveResetSurvivingState();
bool radioOffMode();
bool loadDeviceIdentity();
void makeUuid(char* output);
uint64_t elapsedNowMs();
bool networkBudgetAvailable(uint32_t reserveMs = 0);
