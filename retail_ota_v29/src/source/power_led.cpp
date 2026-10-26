#include "../include/power_led.h"

#include <Ticker.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>

#include "../include/app_state.h"
#include "../include/range_sensor.h"

static Ticker ticker;

// --------------------------------------------------
// LED
// --------------------------------------------------

static bool ledFeedbackEnabled() {
  return LED_PIN >= 0 && (LED_ON_TIMER_WAKE || !timerWake);
}

static void setLed(bool on) {
  if (LED_PIN < 0) return;

  digitalWrite(
    LED_PIN,
    (on != LED_ACTIVE_LOW) ? HIGH : LOW);
}

static void tick() {
  if (LED_PIN >= 0) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
}

void stopLedBlink() {
  ticker.detach();
  setLed(false);
}

void startLedBlink(float seconds) {
  ticker.detach();
  if (!ledFeedbackEnabled()) return;

  setLed(true);
  ticker.attach(seconds, tick);
}

void ledSolidOn() {
  ticker.detach();
  setLed(true);
}

void initializeLed() {
  if (LED_PIN < 0) return;

  // Release the OFF state held during the previous deep sleep.
  gpio_hold_dis((gpio_num_t)LED_PIN);
  gpio_deep_sleep_hold_dis();

  pinMode(LED_PIN, OUTPUT);
  setLed(false);
}

void flashUploadError() {
  stopLedBlink();
  if (!ledFeedbackEnabled()) return;

  for (int i = 0; i < 3; ++i) {
    setLed(true);
    delay(70);
    setLed(false);
    delay(70);
  }
}

// --------------------------------------------------
// Deep sleep
// --------------------------------------------------

void goToDeepSleep() {
  stopLedBlink();
  shutDownSensor();

  // Disable the radio without deleting saved credentials.
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);

  // Hold built-in active-low LED OFF throughout deep sleep.
  if (LED_PIN >= 0) {
    setLed(false);

    esp_err_t err = gpio_hold_en((gpio_num_t)LED_PIN);
    if (err != ESP_OK) {
      LOGF(
        "LED hold failed: %s\n",
        esp_err_to_name(err));
    }
  }

  if (USE_XSHUT) {
    esp_err_t err = gpio_hold_en((gpio_num_t)XSHUT_PIN);
    if (err != ESP_OK) {
      LOGF(
        "XSHUT hold failed: %s\n",
        esp_err_to_name(err));
    }
  }

  gpio_deep_sleep_hold_en();

  uint32_t awakeMs = millis() - cycleStartMs;
  uint32_t targetMs = sleepInterval * 1000UL;
  uint32_t sleepMs = awakeMs < targetMs ? targetMs - awakeMs : MIN_SLEEP_MS;
  if (sleepMs < MIN_SLEEP_MS) sleepMs = MIN_SLEEP_MS;
  retained.previousAwakeMs = awakeMs;
  retained.elapsedMs += (uint64_t)awakeMs + sleepMs;
  LOGF("Awake: %lu ms; target interval: %lu s; sleep: %lu ms\n",
       (unsigned long)awakeMs, (unsigned long)sleepInterval, (unsigned long)sleepMs);
  esp_sleep_enable_timer_wakeup((uint64_t)sleepMs * 1000ULL);

#if ENABLE_SERIAL_LOG
  Serial.flush();
#endif
  esp_deep_sleep_start();
}
