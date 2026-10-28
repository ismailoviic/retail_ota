#pragma once

// Battery voltage and USB charger detection through the 10k/10k dividers — v23 method.
// Read before the radio starts: Wi-Fi current would pull the battery reading down.

void beginBatteryAdc();
float readBattery();      // Volts at the battery
bool readPluginStatus();  // True when the charger input is present
