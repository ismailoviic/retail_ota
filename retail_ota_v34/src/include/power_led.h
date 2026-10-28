#pragma once

// Status LED and deep sleep.

void initializeLed();
void startLedBlink(float seconds);
void stopLedBlink();
void ledSolidOn();  // Configuration portal: steady LED.
void flashUploadError();

#include <stdint.h>

// Turns the radio off, stops the watchdogs, saves the reset-surviving state, holds pins and
// sleeps for what is left of sleepInterval. Never returns.
void goToDeepSleep();
// Awake deadline only: straight to deep sleep for intervalSeconds, no radio teardown.
void abandonWakeToDeepSleep(uint32_t intervalSeconds);
