/*
 AI Coffee V4 / ESP32-C3 / firmware 34
 Power-optimized revision of retail_ota firmware 28; pins, ADC ratios and OTA paths retained.
 Readings are published over MQTT only. Firmware 32 also posted to Supabase, which rejected
 every row with HTTP 400; that upload is removed.
 Firmware 34:
 - No measured_at: the subscriber stamps each message with the server's received_at (NTP
   failed for 10 h after a power-on on 27-28 Oct and let the clock run 2.5 min ahead).
 - Schedule: 30 min online readings down to 15 %, then 1 min readings with the radio off into
   an offline track. A jump of > 15 points starts a refill check (1 min, radio off): 5
   consecutive readings within 1 point confirm it, then the reading and the track are
   published and the device holds 4 h, or until 06:00 UTC after a refill at 17:00-05:59 UTC
   with >= 50 % (NTP on that wake only). A hand or scoop under the sensor fails the check.
 - Watchdog: awake deadline (deep sleep, counters kept) plus the RTC watchdog (chip reset);
   the stuck stage is published once as hang_stage. 27 Oct: frozen until a manual reset.
 - Mode, refill check and last 5 fills survive a watchdog/software reset (and usually RESET).
 - OTA over plain HTTP from admin-panel; installed only if the binary matches its SHA-256.
 - Serial logging off for release.
 Device UUID provisioned from the user's successful database migration.

 Calibration matches the dashboard: fill = (30 cm - distance) / 30 cm (HEIGHT percent).
 Re-measure FULL/EMPTY if the sensor mounting or hopper changes; without calibration the
 device falls back to a fixed 600-second cycle with fill_percent=null.

 Files (this sketch holds only the wake cycle). Headers in src/include, sources in
 src/source; Arduino compiles only the sketch folder and src/, so both live under src/.
   app_config.h             all constants: pins, calibration, schedule, MQTT, OTA, logging
   app_state.h/.cpp         RTC state kept across deep sleep, reset-surviving copy, device ID
   awake_watchdog.h/.cpp    awake deadline, RTC watchdog, stage of a hang
   power_led.h/.cpp         status LED and deep sleep
   battery_adc.h/.cpp       battery voltage and USB charger detection
   range_sensor.h/.cpp      VL53L0X, v28 median of 10 samples
   wake_schedule.h/.cpp     modes, refill check, night hold (NTP), next wake interval
   track_store.h/.cpp       offline track of 1 min fills in RTC memory
   wifi_link.h/.cpp         cached-AP reconnect, full scan, setup portal, backoff
   reading_upload.h/.cpp    grouped JSON reading and track payloads
   mqtt_publish.h/.cpp      plain MQTT publish, QoS 1
   ota_update.h/.cpp        daily firmware check, SHA-256 verified install over HTTP

 Changes from 28 (firmware 29 without its sensor grading):
 - Supabase REST replaced by plain MQTT 3.1.1 (TCP 1883, no TLS, QoS 1) to
   storeyes/<device_id>/coffee-counter as grouped JSON (device, reading, level, sensor, power,
   schedule, network, diagnostics). Requires the Arduino "MQTT" library by Joel Gaehwiler
   (256dpi) and a subscriber that stores readings; the Supabase dashboard gets no new rows.
 - Sensor: v28 filter (range status 0, 20-2000 mm, median, >= 3 valid) with 10 samples instead
   of 5, I2C at 100 kHz. Firmware 29's sigma grading reported no distance on the deployed board.
 - OTA version check once per 24 h (and after every power-on/RESET), not every 10 min.
 - Wi-Fi fast reconnect: last BSSID/channel kept in RTC memory and applied as a RAM-only
   Wi-Fi config, so stored credentials are never rewritten; full scan if the AP moved.
 - Battery/USB ADC read before the radio starts; Wi-Fi associates while the VL53L0X ranges.
 - Wi-Fi backoff: after 3 failed timer wakes, one attempt per hour until it recovers.
 - Timer wakes: no 300 ms serial delay, no LED.
 - Refill-aware schedule (replaces 600/300/60 s by fill level); firmware 34's version above.
   Each MQTT reading carries its schedule_reason.
 - Payloads in one static buffer, device ID cached in RTC memory, 32-bit RTC counters,
   compile-time serial logging switch (ENABLE_SERIAL_LOG).

 Not changed (hardware / infrastructure):
 - The 10k/10k battery divider draws ~185 uA permanently; use 1M/1M + 100 nF or switch it.
 - XSHUT is not wired, so the VL53L0X stays powered during deep sleep.
 - OTA images are not signed; the SHA-256 comes from the same server as the binary.
 - MQTT is unencrypted: readings and broker credentials can be read or forged on the path.

 Counters survive deep sleep, not loss of power/reset; session UUID changes on reset.
 The offline track is lost on any reset except its last 5 fills; a power cut loses all state.
 Online readings have no queue: failed uploads leave sequence gaps.
 Arduino exports this sketch as retail_ota_v34.ino.bin: rename it to retail_ota.ino.bin
 under build/esp32.esp32.esp32c3/ for OTA. Test via USB before publishing an OTA binary.
 Publishing order: binary, then retail_ota.ino.bin.sha256, then version.txt. Firmware 33
 reads GitHub only, so firmware 34 goes to GitHub and to admin-panel.
*/

#include <esp_sleep.h>
#include <esp_system.h>

#include "src/include/app_config.h"
#include "src/include/app_state.h"
#include "src/include/awake_watchdog.h"
#include "src/include/battery_adc.h"
#include "src/include/ota_update.h"
#include "src/include/power_led.h"
#include "src/include/range_sensor.h"
#include "src/include/reading_upload.h"
#include "src/include/track_store.h"
#include "src/include/wake_schedule.h"
#include "src/include/wifi_link.h"

// --------------------------------------------------
// Main cycle — ADC, then ranging (overlapped with Wi-Fi association when online), then
// upload of the reading and any offline track
// --------------------------------------------------

void setup() {
  cycleStartMs = millis();
  timerWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
#if ENABLE_SERIAL_LOG
  Serial.begin(115200);
  if (!timerWake) delay(300);  // Lets a USB monitor attach after power-on/RESET only.
#endif
  initializeLed();
  prepareSensorShutdown();
  initRetainedState();
  startWatchdogs(radioOffMode() ? RADIO_OFF_DEADLINE_MS : ONLINE_DEADLINE_MS);
  if (!loadDeviceIdentity()) {
    LOGLN("Device identity storage failed; upload skipped. Check NVS/provisioning.");
    goToDeepSleep();
    return;
  }
  ++retained.wakes;
  ++retained.sequence;  // One logical measurement per wake, including failures.
  LOGF("\nAI Coffee ESP32-C3 firmware %d; device %s\n", CURRENT_VERSION, deviceId);
  LOGF("Reset %d; wake cause %d; wake %lu\n", (int)esp_reset_reason(),
       (int)esp_sleep_get_wakeup_cause(), (unsigned long)retained.wakes);
  if (!calibrationValid()) LOGLN("Calibration not confirmed: fixed 600 s, fill_percent=null.");

  startLedBlink(0.2f);

  // ADC first, with the radio off: Wi-Fi current would pull the battery reading down.
  beginBatteryAdc();
  float battery = readBattery();
  bool plugged = readPluginStatus();

  // Online: association and DHCP continue in the background while the VL53L0X ranges.
  // Radio-off modes (offline, refill_confirm) start Wi-Fi only if this reading must be sent.
  bool wifiAttempted = !radioOffMode();
  bool wifiStarted = false;
  if (wifiAttempted) {
    setWakeStage(STAGE_WIFI);
    wifiStarted = startWiFi();
  }

  setWakeStage(STAGE_SENSOR);
  beginRangeSensor();
  float distance = readDistance();
  sampleTakenMs = millis();
  shutDownSensor();
  selectedInterval = chooseInterval(distance);
  sleepInterval = selectedInterval;
  LOGF("Distance %.2f cm; fill %.2f (negative=unknown); interval %lu s (%s)\n",
       distance, fillPercent, (unsigned long)selectedInterval, scheduleReason);

  if (!publishThisWake) {
    goToDeepSleep();  // Radio off; a started association is dropped
    return;
  }
  setWakeStage(STAGE_WIFI);
  if (!wifiAttempted) {
    extendAwakeDeadline(ONLINE_DEADLINE_MS);
    wifiStarted = startWiFi();
  }
  if (!wifiStarted || !finishWiFi()) {
    retained.previousPublishResult = 0;  // No publish attempted during this wake.
    ++retained.failedWifiWakes;
    // Backoff only online: radio-off modes keep their 1 min readings.
    if (retained.mode == MODE_ONLINE && retained.failedWifiWakes >= WIFI_BACKOFF_AFTER_FAILED_WAKES
        && sleepInterval < WIFI_BACKOFF_INTERVAL) {
      sleepInterval = WIFI_BACKOFF_INTERVAL;
    }
    LOGF("Wi-Fi unavailable (%lu failed wakes); sleeping. RESET opens setup after connection attempts.\n",
         (unsigned long)retained.failedWifiWakes);
    goToDeepSleep();
    return;
  }
  retained.failedWifiWakes = 0;
  if (nightCheckDue) {
    setWakeStage(STAGE_NTP);
    selectedInterval = applyNightHold(selectedInterval);
    sleepInterval = selectedInterval;
  }

  setWakeStage(STAGE_MQTT);
  startLedBlink(0.10f);
  bool sent = publishReading(distance, battery, plugged);
  if (sent && trackCount() > 0) sent = publishTrack();
  stopLedBlink();
  if (!sent) flashUploadError();

  setWakeStage(STAGE_OTA);
  checkForUpdatesIfDue();
  goToDeepSleep();
}

void loop() {
  goToDeepSleep();
}
