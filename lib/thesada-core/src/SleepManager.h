// thesada-fw - SleepManager.h
// Deep sleep orchestrator. Stays awake for wake_s, then sleeps for sleep_s.
// Graceful shutdown: flushes MQTT, publishes status, disconnects WiFi.
// RTC memory persists boot count and last OTA check time across sleep cycles.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>

// Stored in RTC slow memory - survives deep sleep but not power cycle.
struct RtcData {
  uint32_t magic;          // 0xDEADBEEF = valid; anything else = uninitialised
  uint32_t bootCount;
  time_t   lastOtaCheck;
};

class SleepManager {
public:
  // Read config and set wake deadline.
  // in: none. out: none. Call once at end of setup().
  static void begin();

  // When wake time expires, trigger graceful shutdown and sleep.
  // in: none. out: none. Call every loop().
  static void loop();

  // in: none. out: true if sleep is enabled in config.
  static bool enabled();

  // Boot count from RTC memory - resets on power cycle, survives sleep.
  // in: none. out: boot count.
  static uint32_t bootCount();

  // in: none. out: UTC epoch of last OTA check from RTC memory.
  static time_t lastOtaCheck();

  // Update the OTA check time in RTC memory.
  // in: UTC epoch. out: none.
  static void setLastOtaCheck(time_t t);

private:
  static void gracefulShutdown();

  static bool     _enabled;
  static uint64_t _sleepUs;  // sleep_s past 4294 overflows 32 bits
  static uint32_t _wakeStart;   // millis() at begin
  static uint32_t _wakeSpanMs;  // awake window, wake_s capped (sleep_policy.h)
};
