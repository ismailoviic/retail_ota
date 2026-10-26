#pragma once

// VL53L0X time-of-flight distance sensor: optical quality-weighted ranging.

#include <stdint.h>

// Diagnostics of the last readDistance(); negative = not measured.
extern int sampleCount;   // Inliers used, or accepted samples when the reading failed
extern int sampleSpread;  // mm between the lowest and highest inlier
extern float rangeSigmaMm;
extern float rangeSignalMcps;
extern float rangeAmbientMcps;
extern uint32_t rangeBudgetMs;  // 100, or 200 after a weak-return re-measurement
extern const char* sensorError;

void prepareSensorShutdown();
// Starts I2C and the sensor, retrying a failed initialization once.
void beginRangeSensor();
// Distance in cm, or DISTANCE_UNAVAILABLE_CM.
float readDistance();
void shutDownSensor();
