#pragma once

// Builds the grouped JSON reading and publishes it over plain MQTT with QoS 1.
// Payload groups: device, reading, level, sensor, power, schedule, network, diagnostics;
// field names inside the groups are the v28 column names.

// Requires Wi-Fi to be connected. True once the broker acknowledged the reading.
bool publishReading(float distance, float battery, bool plugged);
