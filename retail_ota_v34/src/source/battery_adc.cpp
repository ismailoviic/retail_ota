#include "../include/battery_adc.h"

#include "../include/app_config.h"

void beginBatteryAdc() {
  pinMode(BATTERY_PIN, INPUT);
  pinMode(PLUGIN_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
  analogSetPinAttenuation(PLUGIN_PIN, ADC_11db);
}

static float median3(float a, float b, float c) {
  if (a > b) {
    float t = a;
    a = b;
    b = t;
  }
  if (b > c) {
    float t = b;
    b = c;
    c = t;
  }
  if (a > b) {
    float t = a;
    a = b;
    b = t;
  }

  return b;
}

static float readPinVoltage(int pin) {
  // Discard first reading after changing ADC channels. The 10k/10k source needs no settle delay;
  // a high-impedance divider would need a 100 nF capacitor at the ADC pin.
  analogReadMilliVolts(pin);

  float values[3];

  for (int i = 0; i < 3; ++i) {
    values[i] = analogReadMilliVolts(pin) / 1000.0f;
  }

  return median3(values[0], values[1], values[2]);
}

float readBattery() {
  float pinV = readPinVoltage(BATTERY_PIN);

  float batteryV =
    pinV * (1.0f + BATTERY_R_TOP / BATTERY_R_BOTTOM)
      * BATTERY_GAIN
    + BATTERY_OFFSET_V;

  LOGF(
    "Battery ADC: %.3f V; reported battery: %.3f V\n",
    pinV,
    batteryV);

  return batteryV;
}

bool readPluginStatus() {
  float pinV = readPinVoltage(PLUGIN_PIN);

  float inputV =
    pinV * (1.0f + USB_R_TOP / USB_R_BOTTOM);

  LOGF(
    "USB ADC: %.3f V; estimated charger input: %.3f V\n",
    pinV,
    inputV);

  return inputV >= USB_PRESENT_THRESHOLD_V;
}
