#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include "Adafruit_VL53L0X.h"
#include <WiFiManager.h>  // Gère le mode AP et la sauvegarde du Wi-Fi
#include <Ticker.h>       // Permet de faire clignoter la LED sans utiliser delay()

// --- Pin Allocations & Constants ---
const int batteryPin = 35; 
const int pluginPin = 34;  
const float REF_VOLTAGE = 3.7;
const float VOLTAGE_DIVIDER_MULTIPLIER = 2.0;
const float VOLTAGE_CALIBRATION_OFFSET = -0.10; // Correction logicielle mesurée au multimètre
const int LED_PIN = 2;  // La LED bleue intégrée à l'ESP32

// --- Supabase Credentials ---
const char* supabaseUrl = "https://yvgsorxwofgpkshlczlm.supabase.co/rest/v1/sensor_data";
const char* supabaseAnonKey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Inl2Z3Nvcnh3b2ZncGtzaGxjemxtIiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODIxOTc2ODgsImV4cCI6MjA5Nzc3MzY4OH0.MgrGm-zTR5DIv2rwmBH0S3rMEDUHA_0ol-Ej43lHBk8";

// --- OTA & GitHub Settings ---
const String versionUrl = "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/version.txt";
const String firmwareUrl = "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/build/esp32.esp32.esp32/retail_ota.ino.bin";

int currentVersion = 17; // VERSION DE PRODUCTION FINALE (V17)

// --- Objects ---
Adafruit_VL53L0X lox = Adafruit_VL53L0X();
Ticker ticker;

// --- Deep Sleep Settings ---
#define uS_TO_S_FACTOR 1000000ULL
#define TIME_TO_SLEEP 600  // PRODUCTION MODE : 600 secondes (10 minutes) de sommeil

// --- Fonctions pour la LED ---
void tick() {
  digitalWrite(LED_PIN, !digitalRead(LED_PIN));
}

void configModeCallback(WiFiManager* myWiFiManager) {
  Serial.println("Échec de connexion au Wi-Fi sauvegardé. Passage en mode AP (Portail Captif)...");
  ticker.detach();
  digitalWrite(LED_PIN, HIGH);
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // Initialisation des broches
  pinMode(LED_PIN, OUTPUT);
  pinMode(pluginPin, INPUT); 

  Serial.println("\n--- Waking up from Deep Sleep ---");

  ticker.attach(0.2, tick);
  analogReadResolution(12);

  // Initialize VL53L0X sensor
  if (!lox.begin()) {
    Serial.println(F("Failed to boot VL53L0X! Check your wiring."));
  } else {
    Serial.println(F("VL53L0X successfully initialized."));
  }

  // 1. Read Sensors & Status (Avec Filtre Médian)
  float distance = readDistance();
  Serial.print("distance: "); Serial.println(distance);
  
  float batteryVoltage = readBattery();
  Serial.print("batteryVoltage: "); Serial.println(batteryVoltage);

  bool isPluggedIn = readPluginStatus();
  Serial.print("isPluggedIn: "); Serial.println(isPluggedIn ? "Yes" : "No");

  // 2. Gestion Intelligente du Wi-Fi (WiFiManager)
  WiFiManager wm;
  wm.setDebugOutput(false);
  wm.setConnectTimeout(15);
  wm.setConfigPortalTimeout(180); // 3 minutes de timeout AP
  wm.setAPCallback(configModeCallback);

  if (!wm.autoConnect("AI Coffee", "12345678")) {
    Serial.println("Timeout de 3 minutes atteint sans nouvelles credentials.");
    digitalWrite(LED_PIN, LOW);
    Serial.println("Échec de la connexion. Retour en sommeil.");
    
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.flush();
    
    // Auto-Recovery : se réveille dans 10 minutes pour retenter
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
  }

  ticker.detach();
  digitalWrite(LED_PIN, LOW);
  Serial.println("Connecté au Wi-Fi avec succès !");

  // 3. Send Data to Supabase (Envoi unique pour la production)
  sendDataToSupabase(distance, batteryVoltage, isPluggedIn, currentVersion);

  // 4. Check for OTA Updates
  checkForUpdates();

  // 5. Return to Deep Sleep
  goToDeepSleep();
}

void loop() {
  // Empty for deep sleep
}

// ========================================================
// Filtre Médian pour la Distance
// ========================================================
float readDistance() {
  float readings[3];
  int validCount = 0;
  
  for (int i = 0; i < 3; i++) {
    VL53L0X_RangingMeasurementData_t measure;
    lox.rangingTest(&measure, false); 
    
    if (measure.RangeStatus != 4) {
      readings[validCount] = measure.RangeMilliMeter / 10.0;
      validCount++;
    }
    delay(50); 
  }

  if (validCount == 0) return 999.0;

  if (validCount < 3) {
    float sum = 0;
    for (int i = 0; i < validCount; i++) sum += readings[i];
    return sum / validCount;
  }

  float diff1 = abs(readings[0] - readings[1]);
  float diff2 = abs(readings[1] - readings[2]);
  float diff3 = abs(readings[0] - readings[2]);
  
  if (max(diff1, max(diff2, diff3)) <= 1.5) {
    return (readings[0] + readings[1] + readings[2]) / 3.0;
  } else {
    for(int i=0; i<2; i++) {
      for(int j=i+1; j<3; j++) {
        if(readings[i] > readings[j]) {
          float temp = readings[i];
          readings[i] = readings[j];
          readings[j] = temp;
        }
      }
    }
    return readings[1];
  }
}

// ========================================================
// Filtre Médian + Calibration pour la Batterie
// ========================================================
float readBattery() {
  float voltageReadings[3];
  
  for (int i = 0; i < 3; i++) {
    int rawADC = analogRead(batteryPin);
    float pinVoltage = (rawADC / 4095.0) * REF_VOLTAGE;
    voltageReadings[i] = (pinVoltage * VOLTAGE_DIVIDER_MULTIPLIER) + VOLTAGE_CALIBRATION_OFFSET;
    delay(10); 
  }

  for(int i=0; i<2; i++) {
    for(int j=i+1; j<3; j++) {
      if(voltageReadings[i] > voltageReadings[j]) {
        float temp = voltageReadings[i];
        voltageReadings[i] = voltageReadings[j];
        voltageReadings[j] = temp;
      }
    }
  }
  
  return voltageReadings[1];
}

bool readPluginStatus() {
  int rawADC = analogRead(pluginPin);
  return (rawADC > 1000); 
}

void sendDataToSupabase(float distance, float battery, bool isPlugged, int version) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, supabaseUrl);

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", supabaseAnonKey);
  http.addHeader("Authorization", String("Bearer ") + String(supabaseAnonKey));

  String jsonPayload = "{\"distance\":" + String(distance, 2) + 
                       ",\"battery_voltage\":" + String(battery, 2) + 
                       ",\"is_plugged\":" + (isPlugged ? "true" : "false") + 
                       ",\"firmware_version\":" + String(version) + "}";

  int httpResponseCode = http.POST(jsonPayload);

  if (httpResponseCode > 0) {
    Serial.printf("Supabase Push Success! Code: %d\n", httpResponseCode);
  } else {
    Serial.printf("Error pushing to Supabase: %s\n", http.errorToString(httpResponseCode).c_str());
  }
  http.end();
}

void checkForUpdates() {
  Serial.println("Checking GitHub for updates...");
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, versionUrl);

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    int latestVersion = payload.toInt();

    if (latestVersion > currentVersion) {
      Serial.println("New version found! Starting OTA update...");
      t_httpUpdate_return ret = httpUpdate.update(client, firmwareUrl);
    } else {
      Serial.println("Firmware is up to date.");
    }
  } else {
    Serial.println("Failed to check GitHub version.");
  }
  http.end();
}

void goToDeepSleep() {
  Serial.println("Going to sleep...");
  digitalWrite(LED_PIN, LOW); 
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  Serial.flush();
  esp_deep_sleep_start();
}