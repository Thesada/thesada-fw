// thesada-fw - SleepManager.h
// Deep sleep orchestrator. Stays awake for wake_s, then sleeps for sleep_s.
// Graceful shutdown: flushes MQTT, publishes status, disconnects WiFi.
// RTC memory persists boot count and last OTA check time across sleep cycles.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>

// Stored in RTC slow memory - survives deep sleep but not power cycle.
struct RtcData {
  uint32_t magic;          // 0xDEADBEEF = valid
  uint32_t bootCount;
  time_t   lastOtaCheck;   // UTC epoch of last OTA check
};

class SleepManager {
public:
  // Call once at end of setup(). Reads config, sets wake deadline.
  static void begin();

  // Call every loop(). When wake time expires, triggers graceful shutdown + sleep.
  static void loop();

  // True if sleep is enabled in config.
  static bool enabled();

  // Boot count from RTC memory (survives sleep, resets on power cycle).
  static uint32_t bootCount();

  // Last OTA check epoch from RTC memory.
  static time_t lastOtaCheck();

  // Update the OTA check time in RTC memory (called by OTAUpdate after a check).
  static void setLastOtaCheck(time_t t);

private:
  static void gracefulShutdown();

  static bool     _enabled;
  static uint32_t _sleepUs;
  static uint32_t _wakeDeadline;  // millis() at which we go to sleep
};
