#pragma once

// State shared by the modules: what survives deep sleep, and this wake's cycle values.

#include "app_config.h"

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
  // Fill history ring for refill and stability detection; fillHead is the next slot.
  uint8_t fillCount;
  uint8_t fillHead;
  int16_t fillTenths[FILL_HISTORY];   // fill % x 10
  uint32_t fillMinute[FILL_HISTORY];  // elapsed minutes at each reading
  uint64_t elapsedMs;
  uint64_t nextOtaMs;
  uint64_t lastNtpElapsedMs;
  uint8_t timeSynced;
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

// Keeps the RTC state only across deep sleep with this firmware's layout.
void initRetainedState();
bool loadDeviceIdentity();
void makeUuid(char* output);
uint64_t elapsedNowMs();
bool networkBudgetAvailable(uint32_t reserveMs = 0);
