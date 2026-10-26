#include "../include/range_sensor.h"

#include <Wire.h>
#include <Adafruit_VL53L0X.h>
#include <driver/gpio.h>

#include "../include/app_config.h"

int sampleCount = 0;
int sampleSpread = -1;
float rangeSigmaMm = -1;
float rangeSignalMcps = -1;
float rangeAmbientMcps = -1;
uint32_t rangeBudgetMs = 0;
const char* sensorError = "not_measured";

static Adafruit_VL53L0X lox;
static bool sensorReady = false;

struct RangeSample {
  uint16_t mm;
  float sigmaMm;
  float signalMcps;   // Returned laser light: drops with dark beans and distance.
  float ambientMcps;  // Background IR (sunlight, lamps) seen by the SPAD array.
};

struct RangeResult {
  float mm;  // NAN when too few samples survive
  int inliers;
  int spreadMm;
  float sigmaMm;
  float signalMcps;
  float ambientMcps;
};

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
  Wire.setTimeOut(100);
  initializeSensor();
  // readDistance escalates a weak optical return itself; only a failed init is retried here.
  if (!sensorReady) {
    LOGLN("Retrying sensor initialization once.");
    delay(50);
    initializeSensor();
  }
}

// --------------------------------------------------
// Sampling and combination
// --------------------------------------------------

// Appends accepted samples to out; returns how many were added.
static int collectRangeSamples(RangeSample* out, int attempts) {
  int added = 0;

  // Single-shot ranging already blocks until each measurement completes.
  for (int i = 0; i < attempts; ++i) {
    VL53L0X_RangingMeasurementData_t measurement = {};

    VL53L0X_Error error =
      lox.getSingleRangingMeasurement(&measurement, false);

    // Sigma of this ranging, as computed by the sensor for its own limit check.
    FixPoint1616_t sigmaFixed = 0;
    bool sigmaKnown =
      error == VL53L0X_ERROR_NONE
      && lox.getLimitCheckCurrent(VL53L0X_CHECKENABLE_SIGMA_FINAL_RANGE, &sigmaFixed, false) == VL53L0X_ERROR_NONE;
    float sigmaMm = sigmaKnown ? sigmaFixed / 65536.0f : RANGE_SIGMA_MAX_MM;
    float signalMcps = measurement.SignalRateRtnMegaCps / 65536.0f;
    float ambientMcps = measurement.AmbientRateRtnMegaCps / 65536.0f;

    bool accepted =
      error == VL53L0X_ERROR_NONE && measurement.RangeStatus == 0
      && measurement.RangeMilliMeter > RANGE_MIN_MM && measurement.RangeMilliMeter < RANGE_MAX_MM
      && sigmaMm <= RANGE_SIGMA_MAX_MM;

    LOGF(
      "  %u mm / status %u / API %d / sigma %.1f mm / signal %.2f / ambient %.2f MCPS %s\n",
      (unsigned)measurement.RangeMilliMeter,
      (unsigned)measurement.RangeStatus,
      (int)error,
      sigmaMm,
      signalMcps,
      ambientMcps,
      accepted ? "OK" : "rejected");

    if (accepted) {
      out[added++] = { measurement.RangeMilliMeter, sigmaMm, signalMcps, ambientMcps };
    }
  }

  return added;
}

static float medianOfSorted(const float* values, int count) {
  return (count % 2) ? values[count / 2] : (values[count / 2 - 1] + values[count / 2]) / 2.0f;
}

static void sortFloats(float* values, int count) {
  for (int i = 1; i < count; ++i) {
    float value = values[i];
    int j = i - 1;
    while (j >= 0 && values[j] > value) {
      values[j + 1] = values[j];
      --j;
    }
    values[j + 1] = value;
  }
}

static RangeResult combineRangeSamples(const RangeSample* samples, int count) {
  RangeResult result = { NAN, 0, -1, -1, -1, -1 };
  if (count < RANGE_MIN_VALID_SAMPLES) return result;

  float values[RANGE_SAMPLE_COUNT * 2];
  for (int i = 0; i < count; ++i) values[i] = samples[i].mm;
  sortFloats(values, count);
  float median = medianOfSorted(values, count);

  // Median absolute deviation, scaled to a standard deviation for normal noise.
  for (int i = 0; i < count; ++i) values[i] = fabsf(samples[i].mm - median);
  sortFloats(values, count);
  float window = fmaxf(RANGE_OUTLIER_MIN_MM, 3.0f * 1.4826f * medianOfSorted(values, count));

  // Inverse-variance mean of the inliers: precise samples count more than noisy ones.
  float weightSum = 0, weightedMm = 0, sigmaSum = 0, signalSum = 0, ambientSum = 0;
  uint16_t low = UINT16_MAX, high = 0;
  for (int i = 0; i < count; ++i) {
    const RangeSample& s = samples[i];
    if (fabsf(s.mm - median) > window) continue;
    float sigma = fmaxf(s.sigmaMm, 1.0f);
    float weight = 1.0f / (sigma * sigma);
    weightSum += weight;
    weightedMm += weight * s.mm;
    sigmaSum += s.sigmaMm;
    signalSum += s.signalMcps;
    ambientSum += s.ambientMcps;
    low = min(low, s.mm);
    high = max(high, s.mm);
    ++result.inliers;
  }
  if (result.inliers < RANGE_MIN_VALID_SAMPLES) return result;

  result.mm = weightedMm / weightSum;
  result.spreadMm = high - low;
  result.sigmaMm = sigmaSum / result.inliers;
  result.signalMcps = signalSum / result.inliers;
  result.ambientMcps = ambientSum / result.inliers;
  return result;
}

float readDistance() {
  sampleCount = 0;
  sampleSpread = -1;
  rangeSigmaMm = rangeSignalMcps = rangeAmbientMcps = -1;
  rangeBudgetMs = 0;
  sensorError = "initialization_failed";
  if (!sensorReady) {
    return DISTANCE_UNAVAILABLE_CM;
  }

  RangeSample samples[RANGE_SAMPLE_COUNT * 2];
  LOGLN("Distance samples (100 ms budget):");
  int count = collectRangeSamples(samples, RANGE_SAMPLE_COUNT);
  rangeBudgetMs = RANGE_TIMING_BUDGET_US / 1000;
  RangeResult result = combineRangeSamples(samples, count);

  // Weak or noisy optical return (dark beans, ambient IR, moving target): integrate longer.
  // Both passes are combined; the sigma weights favour the longer integrations.
  bool weak = isnan(result.mm) || result.spreadMm > RANGE_ESCALATE_SPREAD_MM
              || result.sigmaMm > RANGE_ESCALATE_SIGMA_MM;
  if (weak && lox.setMeasurementTimingBudgetMicroSeconds(RANGE_ESCALATED_BUDGET_US)) {
    LOGLN("Weak or noisy return; re-measuring with a 200 ms budget:");
    count += collectRangeSamples(samples + count, RANGE_SAMPLE_COUNT);
    rangeBudgetMs = RANGE_ESCALATED_BUDGET_US / 1000;
    result = combineRangeSamples(samples, count);
  }

  sampleCount = isnan(result.mm) ? count : result.inliers;
  sensorError = "insufficient_valid_samples";
  if (isnan(result.mm)) {
    LOGF(
      "Distance unavailable: %d accepted samples, fewer than %d consistent.\n",
      count,
      RANGE_MIN_VALID_SAMPLES);

    return DISTANCE_UNAVAILABLE_CM;
  }

  sampleSpread = result.spreadMm;
  rangeSigmaMm = result.sigmaMm;
  rangeSignalMcps = result.signalMcps;
  rangeAmbientMcps = result.ambientMcps;
  LOGF(
    "Distance: %.1f mm; inliers %d/%d; spread %d mm; sigma %.1f mm; signal %.2f; ambient %.2f MCPS\n",
    result.mm,
    result.inliers,
    count,
    sampleSpread,
    rangeSigmaMm,
    rangeSignalMcps,
    rangeAmbientMcps);

  sensorError = "none";
  return result.mm / 10.0f;
}
