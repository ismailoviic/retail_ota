#include "../include/mqtt_publish.h"

#include <WiFi.h>
#include <MQTT.h>

#include "../include/app_state.h"

bool mqttPublish(const char* payload, int length) {
  char topic[96];  // storeyes/<36-char id>/coffee-counter = 60 characters
  snprintf(topic, sizeof(topic), "%s%s%s", MQTT_TOPIC_PREFIX, deviceId, MQTT_TOPIC_SUFFIX);
  LOGF("MQTT topic %s; %d bytes\n", topic, length);

  for (int attempt = 1; attempt <= 3; ++attempt) {
    // Stage limits are not a guaranteed total request deadline.
    if (WiFi.status() != WL_CONNECTED || !networkBudgetAvailable(12000)) break;
    WiFiClient net;  // Plain TCP: the payload and any username/password travel unencrypted.
    MQTTClient mqtt(256, MQTT_BUFFER_SIZE);  // Reads only CONNACK/PUBACK.
    mqtt.begin(MQTT_HOST, MQTT_PORT, net);
    mqtt.setOptions(MQTT_KEEPALIVE_SECONDS, true, MQTT_TIMEOUT_MS);
    // Clean session, no will, no subscriptions: connect, publish, wait for PUBACK, disconnect.
    bool acknowledged = mqtt.connect(deviceId, MQTT_USERNAME, MQTT_PASSWORD)
                        && mqtt.publish(topic, payload, length, false, MQTT_QOS);
    lwmqtt_err_t error = mqtt.lastError();
    lwmqtt_return_code_t returnCode = mqtt.returnCode();
    retained.previousPublishResult = acknowledged ? 1 : (error != LWMQTT_SUCCESS ? (int32_t)error : -1);
    if (mqtt.connected()) mqtt.disconnect();
    else net.stop();
    LOGF("MQTT attempt %d/3: %s; error %d; CONNACK code %d\n", attempt,
         acknowledged ? "acknowledged" : "failed", (int)error, (int)returnCode);
    if (acknowledged) return true;
    ++retained.uploadFailures;
    // No repeated credential, authorization, client-ID or size errors.
    bool permanent = error == LWMQTT_BUFFER_TOO_SHORT
                     || (error == LWMQTT_CONNECTION_DENIED && returnCode != LWMQTT_SERVER_UNAVAILABLE);
    if (permanent) break;
    if (attempt < 3 && networkBudgetAvailable(13000)) delay(500 * attempt);
  }
  LOGLN("MQTT reading not acknowledged; no offline queue.");
  return false;
}
