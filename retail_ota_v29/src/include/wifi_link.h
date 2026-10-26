#pragma once

// Wi-Fi: cached-AP fast reconnect, full-scan fallback, setup portal and failure backoff.

#include <stdint.h>

extern uint32_t wifiConnectMs;  // From startWiFi() to connected (or given up)

// Starts association without waiting, so it overlaps the distance measurement.
bool startWiFi();
// Waits for the connection, falling back to a full scan and, after power-on/RESET, the portal.
bool finishWiFi();
