#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

// --- WiFi Credentials ---
const char* ssid = "ZTE_2.4G_XXPP6m";
const char* password = "3KkYfpUH";

// --- OTA & GitHub Settings ---
const String versionUrl = "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/version.txt";
const String firmwareUrl = "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/build/esp32.esp32.esp32/retail_ota.ino.bin";

int currentVersion = 4; // Make sure to increment this in your new code before exporting the .bin!

// --- Deep Sleep Settings ---
#define uS_TO_S_FACTOR 1000000ULL  // Conversion factor for microseconds to seconds
#define TIME_TO_SLEEP  600         // Time ESP32 will go to sleep (in seconds) -> 600s = 10 minutes

void setup() {
  Serial.begin(115200);
  delay(1000); // Brief pause to let the Serial Monitor catch up after waking
  
  Serial.println("\n\n--- Waking up from Deep Sleep ---");
  Serial.print("Current Firmware Version: ");
  Serial.println(currentVersion);

  // 1. Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  // We add a timeout (retries) so the ESP32 doesn't stay awake forever draining the battery if the WiFi router is down
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  // 2. Check for Updates if WiFi connected successfully
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to WiFi!");
    Serial.println("Checking GitHub for updates...");
    checkForUpdates();
  } else {
    Serial.println("\nFailed to connect to WiFi. Skipping update check.");
  }

  // 3. Go back to sleep (Whether OTA failed, succeeded, or there were no updates)
  goToDeepSleep();
}

void loop() {
  // The loop is intentionally left empty. 
  // Deep sleep restarts the ESP32 from setup() every single time it wakes up.
}

void checkForUpdates() {
  WiFiClientSecure client;
  client.setInsecure(); // Bypass SSL certificate validation

  HTTPClient http;
  http.begin(client, versionUrl);
  
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    int latestVersion = payload.toInt();
    
    Serial.print("Latest version on GitHub: ");
    Serial.println(latestVersion);
    
    if (latestVersion > currentVersion) {
      Serial.println("New version found! Starting OTA update...");
      performOTAUpdate(client);
    } else {
      Serial.println("Firmware is up to date.");
    }
  } else {
    Serial.print("Failed to check version. HTTP Error: ");
    Serial.println(httpCode);
  }
  http.end();
}

void performOTAUpdate(WiFiClientSecure &client) {
  t_httpUpdate_return ret = httpUpdate.update(client, firmwareUrl);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("HTTP_UPDATE_FAILED Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("HTTP_UPDATE_NO_UPDATES");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("HTTP_UPDATE_OK");
      // The ESP32 automatically reboots itself upon a successful update.
      // It will wake up as the NEW version, run setup(), realize it is up to date, and go to sleep.
      break;
  }
}

void goToDeepSleep() {
  Serial.println("Going to sleep for 10 minutes...");
  
  // Best practice: disconnect WiFi and turn off the radio to ensure maximum power savings
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  
  // Configure the wake up timer
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  
  // Ensure all Serial data is printed before cutting power
  Serial.flush(); 
  
  // Enter deep sleep
  esp_deep_sleep_start();
}