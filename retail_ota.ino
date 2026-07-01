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
// Modification : On garde le REF_VOLTAGE à 3.7 car la tension de la batterie est plus élevée, 
// mais on ajoute un offset pour calibrer la mesure (0.1V en moins d'après votre retour)
const float REF_VOLTAGE = 3.7;
const float VOLTAGE_DIVIDER_MULTIPLIER = 2.0;
const float VOLTAGE_CALIBRATION_OFFSET = -0.10; // Correction logicielle
const int LED_PIN = 2;  // La LED bleue intégrée à l'ESP32

// --- Supabase Credentials ---
const char* supabaseUrl = "https://yvgsorxwofgpkshlczlm.supabase.co/rest/v1/sensor_data";
const char* supabaseAnonKey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Inl2Z3Nvcnh3b2ZncGtzaGxjemxtIiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODIxOTc2ODgsImV4cCI6MjA5Nzc3MzY4OH0.MgrGm-zTR5DIv2rwmBH0S3rMEDUHA_0ol-Ej43lHBk8";

// --- OTA & GitHub Settings ---
const String versionUrl = "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/version.txt";
const String firmwareUrl = "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/build/esp32.esp32.esp32/retail_ota.ino.bin";

int currentVersion = 15; // NOUVELLE VERSION DE DEBUG (V15)

// --- Objects ---
Adafruit_VL53L0X lox = Adafruit_VL53L0X();
Ticker ticker;

// --- Deep Sleep Settings ---
#define uS_TO_S_FACTOR 1000000ULL
#define TIME_TO_SLEEP 60  // MODIFIÉ : 60 secondes (1 minute) pour le debug

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

  // 1. Read Sensors & Status (Avec Filtre Médian)
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

  // NOUVEAUTÉ V14 : Timeout réduit à 3 minutes (180 secondes) pour le mode Point d'Accès (AP)
  wm.setConfigPortalTimeout(180);

  // Fonction appelée si l'ESP32 bascule en mode AP
  wm.setAPCallback(configModeCallback);

  // Tente de se connecter au Wi-Fi mémorisé.
  // Si échec après 15s, crée un réseau Wi-Fi sécurisé
  if (!wm.autoConnect("AI Coffee", "12345678")) {
    Serial.println("Timeout de 3 minutes atteint sans nouvelles credentials.");
    // NOUVEAUTÉ V14 : Auto-Recovery. Au lieu de s'éteindre infiniment,
    // on l'endort juste pour le cycle normal (10 minutes) pour qu'il réessaie plus tard.
    digitalWrite(LED_PIN, LOW);
    Serial.println("Échec de la connexion. Retour en sommeil pour réessayer dans 10 minutes.");
    
    // On libère la RAM Wi-Fi avant de dormir
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.flush();
    
    // Il se réveillera dans 10 minutes et retentera wm.autoConnect()
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
  }

  // Si on arrive ici, c'est que l'ESP32 est connecté au Wi-Fi (soit direct, soit via le portail)
  ticker.detach();
  digitalWrite(LED_PIN, LOW);  // Éteint la LED pour économiser la batterie
  Serial.println("Connecté au Wi-Fi avec succès !");

  // 3. Check for OTA Updates FIRST (Crucial pour pouvoir sortir du mode debug plus tard)
  checkForUpdates();

  // 4. Mode Debug "Live" : Boucle de 60 secondes d'envoi continu
  Serial.println("Début de la session d'envoi en direct (60 secondes)...");
  unsigned long startDebugTime = millis();
  
  // Tourne pendant 60 000 millisecondes (1 minute)
  while (millis() - startDebugTime < 60000) {
    // Lecture fraîche des capteurs
    distance = readDistance();
    batteryVoltage = readBattery();
    isPluggedIn = readPluginStatus();
    
    Serial.print("Live Distance: "); Serial.println(distance);
    
    // Envoi à Supabase
    sendDataToSupabase(distance, batteryVoltage, isPluggedIn, currentVersion);
    
    // Pause de 1 seconde (L'envoi HTTP prend déjà ~1.5s, on aura une donnée toutes les ~2.5s)
    delay(1000);
  }
  Serial.println("Fin de la session live. Préparation au sommeil...");

  // 5. Return to Deep Sleep (Pour 1 minute)
  goToDeepSleep();
}

void loop() {
  // Empty for deep sleep
}

// ========================================================
// NOUVEAUTÉ V14 : Filtre Médian pour la Distance
// ========================================================
float readDistance() {
  float readings[3];
  int validCount = 0;
  
  // Prendre 3 lectures très rapides (espacées de 50ms)
  for (int i = 0; i < 3; i++) {
    VL53L0X_RangingMeasurementData_t measure;
    lox.rangingTest(&measure, false); 
    
    // Si la lecture est valide (RangeStatus != 4)
    if (measure.RangeStatus != 4) {
      readings[validCount] = measure.RangeMilliMeter / 10.0; // Conversion en cm
      validCount++;
    }
    delay(50); 
  }

  // Si on n'a réussi aucune lecture valide
  if (validCount == 0) return 999.0;

  // Si on a 1 ou 2 lectures valides, on fait juste la moyenne simple
  if (validCount < 3) {
    float sum = 0;
    for (int i = 0; i < validCount; i++) sum += readings[i];
    return sum / validCount;
  }

  // Si on a 3 lectures valides, on calcule l'écart max
  float diff1 = abs(readings[0] - readings[1]);
  float diff2 = abs(readings[1] - readings[2]);
  float diff3 = abs(readings[0] - readings[2]);
  
  // Tolérance de 1.5 cm
  if (max(diff1, max(diff2, diff3)) <= 1.5) {
    return (readings[0] + readings[1] + readings[2]) / 3.0;
  } else {
    // Écart trop grand (anomalie). On trie et on prend la médiane.
    for(int i=0; i<2; i++) {
      for(int j=i+1; j<3; j++) {
        if(readings[i] > readings[j]) {
          float temp = readings[i];
          readings[i] = readings[j];
          readings[j] = temp;
        }
      }
    }
    return readings[1]; // Retourne la valeur médiane
  }
}

// ========================================================
// NOUVEAUTÉ V14 : Filtre Médian + Calibration pour la Batterie
// ========================================================
float readBattery() {
  float voltageReadings[3];
  
  // Prendre 3 lectures rapides de l'ADC
  for (int i = 0; i < 3; i++) {
    int rawADC = analogRead(batteryPin);
    float pinVoltage = (rawADC / 4095.0) * REF_VOLTAGE;
    
    // Appliquer le multiplicateur du pont diviseur ET l'offset de calibration
    voltageReadings[i] = (pinVoltage * VOLTAGE_DIVIDER_MULTIPLIER) + VOLTAGE_CALIBRATION_OFFSET;
    delay(10); 
  }

  // Trier les 3 valeurs pour trouver la médiane (Filtre pour éliminer le bruit électrique)
  for(int i=0; i<2; i++) {
    for(int j=i+1; j<3; j++) {
      if(voltageReadings[i] > voltageReadings[j]) {
        float temp = voltageReadings[i];
        voltageReadings[i] = voltageReadings[j];
        voltageReadings[j] = temp;
      }
    }
  }
  
  // Retourner la valeur médiane
  return voltageReadings[1];
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