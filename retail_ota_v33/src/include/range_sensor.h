#pragma once

// VL53L0X time-of-flight distance sensor: v28 median of the valid samples.

#include <stdint.h>

// Diagnostics of the last readDistance(); negative = not measured.
extern int sampleCount;   // Valid samples
extern int sampleSpread;  // mm between the lowest and highest valid sample
extern const char* sensorError;

void prepareSensorShutdown();
// Starts I2C and the sensor, retrying a failed initialization once.
void beginRangeSensor();
// Distance in cm, or DISTANCE_UNAVAILABLE_CM.
float readDistance();
void shutDownSensor();
