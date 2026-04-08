// thesada-fw - SleepManager.cpp
// SPDX-License-Identifier: GPL-3.0-only

#include "SleepManager.h"
#include "Config.h"
#include "Log.h"
#include "Shell.h"
#include "MQTTClient.h"
#include "WiFiManager.h"
#include <WiFi.h>
#include <esp_sleep.h>

static const char* TAG = "Sleep";

// RTC slow memory - persists across deep sleep, cleared on power cycle.
RTC_DATA_ATTR static RtcData _rtc;

bool     SleepManager::_enabled      = false;
uint32_t SleepManager::_sleepUs      = 0;
uint32_t SleepManager::_wakeDeadline = 0;

// Initialize RTC data, read sleep config, and set wake deadline
void SleepManager::begin() {
  // Init RTC data on first boot (power cycle).
  if (_rtc.magic != 0xDEADBEEF) {
    _rtc.magic        = 0xDEADBEEF;
    _rtc.bootCount    = 0;
    _rtc.lastOtaCheck = 0;
  }
  _rtc.bootCount++;

  JsonObject cfg = Config::get();
  _enabled = cfg["sleep"]["enabled"] | false;

  // Register sleep shell command
  Shell::registerCommand("sleep", "Sleep config, boot count",
      [](int argc, char** argv, ShellOutput out) {
        char line[80];
        snprintf(line, sizeof(line), "Sleep: %s  boot #%lu",
                 SleepManager::enabled() ? "enabled" : "disabled",
                 (unsigned long)SleepManager::bootCount());
        out(line);
        if (SleepManager::enabled()) {
          JsonObject cfg = Config::get();
          uint32_t sleepS = cfg["sleep"]["sleep_s"] | 300;
          uint32_t wakeS  = cfg["sleep"]["wake_s"]  | 30;
          snprintf(line, sizeof(line), "  wake %lus  sleep %lus", (unsigned long)wakeS, (unsigned long)sleepS);
          out(line);
          time_t lastOta = SleepManager::lastOtaCheck();
          if (lastOta > 0) {
            snprintf(line, sizeof(line), "  last OTA check: %ld", (long)lastOta);
            out(line);
          }
        }
      });

  if (!_enabled) return;

  uint32_t sleepS = cfg["sleep"]["sleep_s"] | 300;
  uint32_t wakeS  = cfg["sleep"]["wake_s"]  | 30;

  if (sleepS < 10)  sleepS = 10;   // minimum 10s sleep
  if (wakeS  < 10)  wakeS  = 10;   // minimum 10s awake

  _sleepUs      = (uint64_t)sleepS * 1000000ULL;
  _wakeDeadline = millis() + (wakeS * 1000);

  char msg[80];
  snprintf(msg, sizeof(msg), "Enabled - awake %lus, sleep %lus (boot #%lu)",
           (unsigned long)wakeS, (unsigned long)sleepS, (unsigned long)_rtc.bootCount);
  Log::info(TAG, msg);
}

// Enter deep sleep when the wake deadline has passed
void SleepManager::loop() {
  if (!_enabled) return;
  if (millis() < _wakeDeadline) return;

  gracefulShutdown();
}

// Return whether deep sleep mode is enabled
bool SleepManager::enabled() {
  return _enabled;
}

// Return the number of boots since last power cycle
uint32_t SleepManager::bootCount() {
  return _rtc.bootCount;
}

// Return the RTC-persisted timestamp of the last OTA check
time_t SleepManager::lastOtaCheck() {
  return _rtc.lastOtaCheck;
}

// Store the last OTA check timestamp in RTC memory
void SleepManager::setLastOtaCheck(time_t t) {
  _rtc.lastOtaCheck = t;
}

// Flush MQTT, disconnect WiFi, and enter deep sleep
void SleepManager::gracefulShutdown() {
  Log::info(TAG, "Going to sleep...");

  // Publish sleep status and flush the MQTT queue.
  if (MQTTClient::connected()) {
    JsonObject cfg     = Config::get();
    const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
    char topic[64];
    snprintf(topic, sizeof(topic), "%s/status", prefix);
    MQTTClient::publish(topic, "sleeping");

    // Give PubSubClient time to send the queued messages.
    for (int i = 0; i < 20; i++) {
      MQTTClient::loop();
      delay(50);
    }
  }

  // Disconnect WiFi cleanly.
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // Configure wake source: RTC timer.
  esp_sleep_enable_timer_wakeup(_sleepUs);

  Log::info(TAG, "Sleeping now");
  delay(100);  // let serial flush

  esp_deep_sleep_start();
  // Does not return - next boot starts from setup().
}
