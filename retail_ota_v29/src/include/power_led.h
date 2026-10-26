#pragma once

// Status LED and deep sleep.

void initializeLed();
void startLedBlink(float seconds);
void stopLedBlink();
void ledSolidOn();  // Configuration portal: steady LED.
void flashUploadError();

// Turns the radio off, holds pins and sleeps for what is left of sleepInterval. Never returns.
void goToDeepSleep();
