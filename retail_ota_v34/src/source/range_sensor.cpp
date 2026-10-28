#include "../include/range_sensor.h"

#include <Wire.h>
#include <Adafruit_VL53L0X.h>
#include <driver/gpio.h>

#include "../include/app_config.h"

int sampleCount = 0;
int sampleSpread = -1;
const char* sensorError = "not_measured";

static Adafruit_VL53L0X lox;
static bool sensorReady = false;

// --------------------------------------------------
// Optional sensor shutdown
// --------------------------------------------------

void prepareSensorShutdown() {
  if (!USE_XSHUT) return;

  pinMode(XSHUT_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(XSHUT_PIN, LOW);

  gpio_hold_dis((gpio_num_t)XSHUT_PIN);
  gpio_deep_sleep_hold_dis();

  digitalWrite(XSHUT_PIN, LOW);
}

static void wakeSensor() {
  if (!USE_XSHUT) return;

  // Open-drain HIGH releases the pin to the breakout pull-up.
  digitalWrite(XSHUT_PIN, HIGH);
  delay(10);
}

void shutDownSensor() {
  if (!USE_XSHUT) return;

  digitalWrite(XSHUT_PIN, LOW);
  sensorReady = false;
}

// --------------------------------------------------
// Initialization
// --------------------------------------------------

static bool initializeSensor() {
  wakeSensor();

  sensorReady = lox.begin(0x29, false, &Wire);

  if (sensorReady) {
    sensorReady =
      lox.setMeasurementTimingBudgetMicroSeconds(
        RANGE_TIMING_BUDGET_US);
  }

  LOGLN(
    sensorReady
      ? "VL53L0X ready, 100 ms timing budget."
      : "VL53L0X initialization failed; distance will be 999.");

  return sensorReady;
}

void beginRangeSensor() {
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(I2C_CLOCK_HZ);
  initializeSensor();
  if (!sensorReady) {
    LOGLN("Retrying sensor initialization once.");
    delay(50);
    initializeSensor();
  }
}

// --------------------------------------------------
// v28 median filter, 10 samples
// --------------------------------------------------

float readDistance() {
  sampleCount = 0;
  sampleSpread = -1;
  sensorError = "initialization_failed";
  if (!sensorReady) {
    return DISTANCE_UNAVAILABLE_CM;
  }

  uint16_t valid[RANGE_SAMPLE_COUNT];
  int count = 0;

  LOGLN("Distance samples: mm / range status / API status");

  for (int i = 0; i < RANGE_SAMPLE_COUNT; ++i) {
    VL53L0X_RangingMeasurementData_t measurement = {};

    VL53L0X_Error error =
      lox.getSingleRangingMeasurement(&measurement, false);

    bool accepted =
      error == VL53L0X_ERROR_NONE && measurement.RangeStatus == 0
      && measurement.RangeMilliMeter > RANGE_MIN_MM && measurement.RangeMilliMeter < RANGE_MAX_MM;

    LOGF(
      "  %d: %u / %u / %d %s\n",
      i + 1,
      (unsigned)measurement.RangeMilliMeter,
      (unsigned)measurement.RangeStatus,
      (int)error,
      accepted ? "OK" : "rejected");

    if (accepted) {
      valid[count++] = measurement.RangeMilliMeter;
    }

    delay(RANGE_SAMPLE_GAP_MS);
  }

  sampleCount = count;
  sensorError = "insufficient_valid_samples";
  if (count < RANGE_MIN_VALID_SAMPLES) {
    LOGF(
      "Distance unavailable: only %d/%d valid samples.\n",
      count,
      RANGE_SAMPLE_COUNT);

    return DISTANCE_UNAVAILABLE_CM;
  }

  for (int i = 1; i < count; ++i) {
    uint16_t value = valid[i];
    int j = i - 1;

    while (j >= 0 && valid[j] > value) {
      valid[j + 1] = valid[j];
      --j;
    }

    valid[j + 1] = value;
  }

  float medianMM = (count % 2)
                     ? valid[count / 2]
                     : (valid[count / 2 - 1] + valid[count / 2]) / 2.0f;

  sampleSpread = valid[count - 1] - valid[0];
  LOGF(
    "Median: %.1f mm; valid: %d/%d; spread: %d mm\n",
    medianMM,
    count,
    RANGE_SAMPLE_COUNT,
    sampleSpread);

  sensorError = "none";
  return medianMM / 10.0f;
}
