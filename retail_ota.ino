#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

// --- WiFi Credentials ---
const char* ssid = "ZTE_2.4G_XXPP6m";
const char* password = "3KkYfpUH";

// --- Supabase Credentials ---
const char* supabaseUrl = "https://yvgsorxwofgpkshlczlm.supabase.co/rest/v1/sensor_data";
const char* supabaseAnonKey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Inl2Z3Nvcnh3b2ZncGtzaGxjemxtIiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODIxOTc2ODgsImV4cCI6MjA5Nzc3MzY4OH0.MgrGm-zTR5DIv2rwmBH0S3rMEDUHA_0ol-Ej43lHBk8";

// --- OTA & GitHub Settings ---
const String versionUrl = "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/version.txt";
const String firmwareUrl = "https://raw.githubusercontent.com/ismailoviic/retail_ota/main/build/esp32.esp32.esp32/retail_ota.ino.bin";

int currentVersion = 7; // The version currently running

// --- Pin Allocations ---
const int trigPin = 5;      
const int echoPin = 18;     
const int batteryPin = 34;  

// --- Deep Sleep Settings ---
#define uS_TO_S_FACTOR 1000000ULL  
#define TIME_TO_SLEEP  600         // Sleep duration in seconds (10 minute)

void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println("\n--- Waking up from Deep Sleep ---");

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  pinMode(batteryPin, INPUT);

  // 1. Read Sensors
  float distance = readDistance();
  float batteryVoltage = readBattery();
  
  // 2. Connect to WiFi
  WiFi.begin(ssid, password);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    // 3. Send Data to Supabase (Including Version)
    sendDataToSupabase(distance, batteryVoltage, currentVersion);
    
    // 4. Check for OTA Updates
    checkForUpdates();
  } else {
    Serial.println("WiFi Connection Failed. Skipping Supabase and OTA.");
  }

  // 5. Return to Deep Sleep
  goToDeepSleep();
}

void loop() {
  // Empty for deep sleep
}

float readDistance() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  
  long duration = pulseIn(echoPin, HIGH, 30000); 
  if (duration == 0) return 999.0;               
  
  return duration * 0.0343 / 2.0;
}

float readBattery() {
  int rawADC = analogRead(batteryPin);
  float measuredVoltage = (rawADC / 4095.0) * 3.3;
  return measuredVoltage * 2.0; 
}

void sendDataToSupabase(float distance, float battery, int version) {
  WiFiClientSecure client;
  client.setInsecure(); 

  HTTPClient http;
  http.begin(client, supabaseUrl);
  
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", supabaseAnonKey);
  http.addHeader("Authorization", String("Bearer ") + supabaseAnonKey);
  
  // JSON Payload now includes firmware_version
  String jsonPayload = "{\"distance\":" + String(distance, 2) + 
                       ",\"battery_voltage\":" + String(battery, 2) + 
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
      // If successful, ESP32 reboots here automatically.
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
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  Serial.flush();
  esp_deep_sleep_start();
}