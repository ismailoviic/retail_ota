#include "../include/awake_watchdog.h"

#include <esp_attr.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <hal/wdt_hal.h>
#include <soc/rtc.h>

#include "../include/app_state.h"
#include "../include/power_led.h"

static esp_timer_handle_t deadlineTimer = nullptr;
static wdt_hal_context_t rtcWdt = RWDT_HAL_CONTEXT_DEFAULT();

// Survives the RTC watchdog's system reset; the magic is tied to the stage value.
RTC_NOINIT_ATTR static uint8_t stageNow;
RTC_NOINIT_ATTR static uint32_t stageCheck;
static constexpr uint32_t STAGE_MAGIC = 0x57A6E000;

static const char* const STAGE_NAMES[STAGE_COUNT] = {
  nullptr, "boot", "sensor", "wifi", "portal", "ntp", "mqtt", "ota",
};

static void startRtcWatchdog() {
  // The RTC slow clock is the ~136 kHz RC oscillator; a few % off is irrelevant for 10 min.
  uint32_t ticks = (uint32_t)((uint64_t)RTC_WATCHDOG_MS * rtc_clk_slow_freq_get_hz() / 1000ULL);
  wdt_hal_write_protect_disable(&rtcWdt);
  wdt_hal_config_stage(&rtcWdt, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_SYSTEM);
  wdt_hal_config_stage(&rtcWdt, WDT_STAGE1, 0, WDT_STAGE_ACTION_OFF);
  wdt_hal_config_stage(&rtcWdt, WDT_STAGE2, 0, WDT_STAGE_ACTION_OFF);
  wdt_hal_config_stage(&rtcWdt, WDT_STAGE3, 0, WDT_STAGE_ACTION_OFF);
  wdt_hal_enable(&rtcWdt);
  wdt_hal_write_protect_enable(&rtcWdt);
}

static void stopRtcWatchdog() {
  wdt_hal_write_protect_disable(&rtcWdt);
  wdt_hal_disable(&rtcWdt);
  wdt_hal_write_protect_enable(&rtcWdt);
}

static void onDeadline(void*) {
  // Runs in the esp_timer task while the main task is stuck somewhere.
  ++retained.watchdogTrips;
  retained.pendingHangStage = stageNow < STAGE_COUNT && stageNow != STAGE_NONE ? stageNow : STAGE_BOOT;
  stopRtcWatchdog();
  stageNow = STAGE_NONE;
  stageCheck = STAGE_MAGIC;
  abandonWakeToDeepSleep(radioOffMode() ? DEADLINE_SLEEP_RADIO_OFF : DEADLINE_SLEEP_ONLINE);
}

void startWatchdogs(uint32_t deadlineMs) {
  if (!deadlineTimer) {
    esp_timer_create_args_t args = {};
    args.callback = onDeadline;
    args.name = "awake_deadline";
    if (esp_timer_create(&args, &deadlineTimer) != ESP_OK) deadlineTimer = nullptr;
  }
  if (deadlineTimer) esp_timer_start_once(deadlineTimer, (uint64_t)deadlineMs * 1000ULL);
  startRtcWatchdog();
  setWakeStage(STAGE_BOOT);
}

void extendAwakeDeadline(uint32_t deadlineMs) {
  if (deadlineTimer) esp_timer_restart(deadlineTimer, (uint64_t)deadlineMs * 1000ULL);
}

void setWakeStage(WakeStage stage) {
  stageNow = stage;
  stageCheck = STAGE_MAGIC ^ stage;
}

void stopWatchdogs() {
  if (deadlineTimer) esp_timer_stop(deadlineTimer);
  stopRtcWatchdog();
  setWakeStage(STAGE_NONE);
}

uint8_t hangStageBeforeReset() {
  esp_reset_reason_t reason = esp_reset_reason();
  bool watchdogReset = reason == ESP_RST_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT;
  if (!watchdogReset || stageCheck != (STAGE_MAGIC ^ stageNow)) return STAGE_NONE;
  if (stageNow == STAGE_NONE || stageNow >= STAGE_COUNT) return STAGE_NONE;
  return stageNow;
}

const char* wakeStageName(uint8_t stage) {
  return stage < STAGE_COUNT ? STAGE_NAMES[stage] : nullptr;
}
