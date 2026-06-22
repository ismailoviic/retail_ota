#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

// --- WiFi Credentials ---
const char* ssid = "IDS SALE";
const char* password = "IDS@2023";

// --- OTA & GitHub Settings ---
// Must be the RAW urls!
const String versionUrl = "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/version.txt";
const String firmwareUrl = "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/build/esp32.esp32.esp32/retail_ota.ino.bin";

int currentVersion = 1; // The version currently running on the ESP32

// --- Timer Settings ---
unsigned long previousMillis = 0;
const long interval = 300000; // 5 minutes in milliseconds (5 * 60 * 1000)

void setup() {
  Serial.begin(115200);
  Serial.println("Starting ESP32...");
  Serial.print("Current Firmware Version: ");
  Serial.println(currentVersion);

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

  // Check every 5 minutes
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Checking for updates...");
      checkForUpdates();
    } else {
      Serial.println("WiFi disconnected, skipping update check.");
    }
  }

  // Your regular ESP32 code goes here...
}

void checkForUpdates() {
  WiFiClientSecure client;
  client.setInsecure(); // Bypass SSL certificate validation for simplicity

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
  // ESPhttpUpdate handles the downloading and flashing of the .bin file
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
      // The ESP32 will automatically reboot here upon successful update
      break;
  }
}