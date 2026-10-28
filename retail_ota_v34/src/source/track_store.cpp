#include "../include/track_store.h"

#include "../include/app_state.h"

void appendTrackSample(float fill) {
  if (retained.trackCount > 0 && retained.sequence != retained.trackLastSequence + 1) {
    LOGF("Track of %u samples not contiguous; dropped.\n", (unsigned)retained.trackCount);
    ++retained.tracksDropped;
    clearTrack();
  }
  retained.trackTenths[retained.trackHead] = fill < 0 ? -1 : (int16_t)lroundf(fill * 10.0f);
  retained.trackHead = (retained.trackHead + 1) % TRACK_CAPACITY;
  if (retained.trackCount < TRACK_CAPACITY) {
    ++retained.trackCount;
  } else {
    ++retained.trackFullReadings;  // Oldest sample overwritten
  }
  retained.trackLastSequence = retained.sequence;
  retained.trackLastElapsedMs = retained.elapsedMs + (sampleTakenMs - cycleStartMs);
}

int trackCount() {
  return retained.trackCount;
}

int16_t trackSampleTenths(int i) {
  int oldest = (retained.trackHead - retained.trackCount + TRACK_CAPACITY) % TRACK_CAPACITY;
  return retained.trackTenths[(oldest + i) % TRACK_CAPACITY];
}

uint32_t trackFirstSequence() {
  return retained.trackLastSequence - retained.trackCount + 1;
}

uint32_t trackLastSampleAgeMs() {
  return (uint32_t)(elapsedNowMs() - retained.trackLastElapsedMs);
}

bool trackUploadDue() {
  return retained.trackCount >= TRACK_CAPACITY
         && retained.trackFullReadings % TRACK_FULL_RETRY_READINGS == 0;
}

void clearTrack() {
  retained.trackHead = 0;
  retained.trackCount = 0;
  retained.trackFullReadings = 0;
}
