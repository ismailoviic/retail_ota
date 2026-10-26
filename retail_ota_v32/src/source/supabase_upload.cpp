#include "../include/supabase_upload.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "../include/app_state.h"

bool supabaseUpload(const char* payload, int length) {
  char authorization[320];
  snprintf(authorization, sizeof(authorization), "Bearer %s", SUPABASE_ANON_KEY);

  for (int attempt = 1; attempt <= 3; ++attempt) {
    // Stage limits are not a guaranteed total request deadline.
    if (WiFi.status() != WL_CONNECTED || !networkBudgetAvailable(12000)) break;
    WiFiClientSecure client;
    client.setInsecure();  // Preserved from v25; review before production deployment.
    client.setHandshakeTimeout(4);
    HTTPClient http;
    http.setConnectTimeout(HTTP_STAGE_TIMEOUT_MS);
    http.setTimeout(HTTP_STAGE_TIMEOUT_MS);
    if (!http.begin(client, String(SUPABASE_URL) + "?on_conflict=reading_id")) {
      ++retained.uploadFailures;
      break;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", SUPABASE_ANON_KEY);
    http.addHeader("Authorization", authorization);
    http.addHeader("Prefer", "resolution=ignore-duplicates,return=minimal");
    int code = http.POST((uint8_t*)payload, length);
    retained.previousHttp = code;
    LOGF("Supabase attempt %d/3: HTTP %d\n", attempt, code);
    if (code >= 200 && code < 300) {
      http.end();
      return true;
    }
#if ENABLE_SERIAL_LOG
    if (code > 0) Serial.println(http.getString().substring(0, 500));
    else Serial.println(http.errorToString(code));
#endif
    http.end();
    ++retained.uploadFailures;
    bool transient = code < 0 || code == 408 || code == 429 || code >= 500;
    if (!transient) break;  // No repeated schema, credentials or FK errors.
    if (attempt < 3 && networkBudgetAvailable(13000)) delay(500 * attempt);
  }
  LOGLN("Supabase reading not stored; no offline queue.");
  return false;
}
