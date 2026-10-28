#pragma once

// Offline track: one fill value per radio-off reading (1 min apart), kept in RTC memory until
// the broker acknowledges its upload. Times are not stored: sample i of n was taken about
// (n - 1 - i) x OFFLINE_INTERVAL before the newest one.

#include <stdint.h>

// fill < 0 records a sensor failure. A sample that does not directly follow the previous one
// (an online wake in between whose upload failed) drops the old track and starts a new one.
void appendTrackSample(float fill);
int trackCount();
int16_t trackSampleTenths(int i);  // i = 0 is the oldest
uint32_t trackFirstSequence();
uint32_t trackLastSampleAgeMs();
// Full and unsent: upload now (then once per TRACK_FULL_RETRY_READINGS readings).
bool trackUploadDue();
void clearTrack();
