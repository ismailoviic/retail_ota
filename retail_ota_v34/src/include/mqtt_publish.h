#pragma once

// Plain MQTT 3.1.1 (TCP, no TLS) publish with QoS 1 to storeyes/<device_id>/coffee-counter.

// Up to 3 attempts. Records the result in retained.previousPublishResult.
// True once the broker acknowledged (PUBACK).
bool mqttPublish(const char* payload, int length);
