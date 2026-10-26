#pragma once

// Sends each reading twice, with one reading_id: the flat v28 row to Supabase over HTTPS
// (the dashboard's source), then the grouped JSON over plain MQTT with QoS 1.

// Requires Wi-Fi to be connected. True only when both were acknowledged.
bool publishReading(float distance, float battery, bool plugged);
