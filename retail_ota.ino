#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

// --- WiFi Credentials ---
const char* ssid = "ZTE_2.4G_XXPP6m";
const char* password = "3KkYfpUH";

// --- OTA & GitHub Settings ---
// Using your raw GitHub URLs
const String versionUrl = "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/version.txt";
const String firmwareUrl = "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/build/esp32.esp32.esp32/retail_ota.ino.bin";

int currentVersion = 3; // Updated to Version 2!

// --- Timer Settings ---
unsigned long previousOtaMillis = 0;
const long otaInterval = 20000; // 5 minutes (5 * 60 * 1000 ms)

unsigned long previousPrintMillis = 0;
const long printInterval = 10000; // Print version every 10 seconds

void setup() {
  Serial.begin(115200);
  Serial.println("Starting ESP32...");
  
  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi!");
}

void loop() {
  unsigned long currentMillis = millis();

  // 1. Print the current version status every 10 seconds
  if (currentMillis - previousPrintMillis >= printInterval) {
    previousPrintMillis = currentMillis;
    
    Serial.print("updated, the current version is ");
    Serial.println(currentVersion);
  }

  // 2. Check for GitHub updates every 5 minutes
  if (currentMillis - previousOtaMillis >= otaInterval) {
    previousOtaMillis = currentMillis;

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Checking GitHub for updates...");
      checkForUpdates();
    } else {
      Serial.println("WiFi disconnected, skipping update check.");
    }
  }
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
    
    Serial.print("Latest version on GitHub a khay Ismail: ");
    Serial.println(latestVersion);
    
    if (latestVersion > currentVersion) {
      Serial.println("New version found! Starting OTA update...");
      performOTAUpdate(client);
    } else {
      Serial.println("Firmware is up to date. ya salaaaaam a ba Ismail");
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
      // ESP32 automatically reboots upon success
      break;
  }
}