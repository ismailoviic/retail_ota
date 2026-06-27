#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include "Adafruit_VL53L0X.h"
#include <WiFiManager.h>  // Gère le mode AP et la sauvegarde du Wi-Fi
#include <Ticker.h>       // Permet de faire clignoter la LED sans utiliser delay()

// --- Pin Allocations & Constants ---
const int batteryPin = 35; // Déplacé de 34 à 35
const int pluginPin = 34;  // Nouvelle broche pour la détection USB
const float REF_VOLTAGE = 3.7;
const float VOLTAGE_DIVIDER_MULTIPLIER = 2.0;
const int LED_PIN = 2;  // La LED bleue intégrée à l'ESP32

// --- Supabase Credentials ---
const char* supabaseUrl = "https://yvgsorxwofgpkshlczlm.supabase.co/rest/v1/sensor_data";
const char* supabaseAnonKey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Inl2Z3Nvcnh3b2ZncGtzaGxjemxtIiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODIxOTc2ODgsImV4cCI6MjA5Nzc3MzY4OH0.MgrGm-zTR5DIv2rwmBH0S3rMEDUHA_0ol-Ej43lHBk8";

// --- OTA & GitHub Settings ---
const String versionUrl = "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/version.txt";
const String firmwareUrl = "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/build/esp32.esp32.esp32/retail_ota.ino.bin";

int currentVersion = 13; // Mise à jour de la version

// --- Objects ---
Adafruit_VL53L0X lox = Adafruit_VL53L0X();
Ticker ticker;

// --- Deep Sleep Settings ---
#define uS_TO_S_FACTOR 1000000ULL
#define TIME_TO_SLEEP 600  // Sleep duration in seconds (10 minutes)

// --- Fonctions pour la LED ---
void tick() {
  // Alterne l'état de la LED
  digitalWrite(LED_PIN, !digitalRead(LED_PIN));
}

void configModeCallback(WiFiManager* myWiFiManager) {
  Serial.println("Échec de connexion au Wi-Fi sauvegardé. Passage en mode AP (Portail Captif)...");
  // Arrête le clignotement et allume la LED en fixe pour indiquer le mode AP
  ticker.detach();
  digitalWrite(LED_PIN, HIGH);
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // Initialisation des broches
  pinMode(LED_PIN, OUTPUT);
  pinMode(pluginPin, INPUT); // Configuration de la broche de détection USB

  Serial.println("\n--- Waking up from Deep Sleep ---");

  // On démarre le clignotement rapide (0.2s) pour indiquer la recherche Wi-Fi
  ticker.attach(0.2, tick);

  analogReadResolution(12);

  // Initialize VL53L0X sensor
  if (!lox.begin()) {
    Serial.println(F("Failed to boot VL53L0X! Check your wiring."));
  } else {
    Serial.println(F("VL53L0X successfully initialized."));
  }

  // 1. Read Sensors & Status
  float distance = readDistance();
  Serial.print("distance: ");
  Serial.println(distance);
  
  float batteryVoltage = readBattery();
  Serial.print("batteryVoltage: ");
  Serial.println(batteryVoltage);

  // Lecture de l'état de charge
  bool isPluggedIn = readPluginStatus();
  Serial.print("isPluggedIn: ");
  Serial.println(isPluggedIn ? "Yes" : "No");

  // 2. Gestion Intelligente du Wi-Fi (WiFiManager)
  WiFiManager wm;

  // Désactive les longs logs de debug dans la console
  wm.setDebugOutput(false);

  // Temps maximum (en secondes) pour essayer de se connecter au Wi-Fi connu
  wm.setConnectTimeout(15);

  // Timeout de 5 minutes (300 secondes) pour le mode Point d'Accès (AP)
  wm.setConfigPortalTimeout(300);

  // Fonction appelée si l'ESP32 bascule en mode AP
  wm.setAPCallback(configModeCallback);

  // Tente de se connecter au Wi-Fi mémorisé.
  // Si échec après 15s, crée un réseau Wi-Fi sécurisé
  if (!wm.autoConnect("AI Coffee", "12345678")) {
    Serial.println("Timeout de 5 minutes atteint sans nouvelles credentials.");
    // Si on arrive ici, le timeout de 5 minutes est passé sans succès.
    // On force l'extinction de la LED et on part en veille infinie
    digitalWrite(LED_PIN, LOW);

    Serial.println("Passage en veille infinie (réveil manuel uniquement).");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.flush();
    // En n'activant aucun timer de réveil, l'ESP32 dormira pour toujours
    // jusqu'à ce que l'utilisateur coupe et remette l'alimentation via l'interrupteur.
    esp_deep_sleep_start();
  }

  // Si on arrive ici, c'est que l'ESP32 est connecté au Wi-Fi (soit direct, soit via le portail)
  ticker.detach();
  digitalWrite(LED_PIN, LOW);  // Éteint la LED pour économiser la batterie
  Serial.println("Connecté au Wi-Fi avec succès !");

  // 3. Send Data to Supabase (Now includes plug status)
  sendDataToSupabase(distance, batteryVoltage, isPluggedIn, currentVersion);

  // 4. Check for OTA Updates
  checkForUpdates();

  // 5. Return to Deep Sleep
  goToDeepSleep();
}

void loop() {
  // Empty for deep sleep
}

float readDistance() {
  VL53L0X_RangingMeasurementData_t measure;
  lox.rangingTest(&measure, false);

  if (measure.RangeStatus != 4) {
    return measure.RangeMilliMeter / 10.0;
  } else {
    return 999.0;
  }
}

float readBattery() {
  int rawADC = analogRead(batteryPin);
  float pinVoltage = (rawADC / 4095.0) * REF_VOLTAGE;
  return pinVoltage * VOLTAGE_DIVIDER_MULTIPLIER;
}

bool readPluginStatus() {
  // Lit la tension sur la broche 34. 
  // Avec un diviseur de tension 10k/10k, 5V (USB branché) donne 2.5V.
  // L'ADC (0-4095) lira environ 3100 pour 2.5V.
  // On met un seuil de sécurité à 1000 pour détecter la présence de tension.
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

  // Ajout du booléen is_plugged au payload JSON
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
  digitalWrite(LED_PIN, LOW);  // Sécurité supplémentaire pour l'énergie
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  Serial.flush();
  esp_deep_sleep_start();
}