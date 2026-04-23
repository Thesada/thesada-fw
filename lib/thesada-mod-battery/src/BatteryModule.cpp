// thesada-fw - BatteryModule.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include <thesada_config.h>
#include "BatteryModule.h"
#include <Config.h>
#include <EventBus.h>
#include <MQTTClient.h>
#include <Log.h>
#include <Shell.h>
#include <SensorRegistry.h>
#include <PowerManager.h>
#include <ArduinoJson.h>
#include <ModuleRegistry.h>

static const char* TAG = "Battery";

// Load battery config and publish initial state if PMU is available
void BatteryModule::begin() {
  if (!PowerManager::isPmuOk()) {
    Log::error(TAG, "PMU not available - battery monitoring disabled");
    return;
  }

  JsonObject cfg = Config::get();
  bool enabled   = cfg["battery"]["enabled"] | true;
  if (!enabled) {
    Log::info(TAG, "Disabled via config");
    _disabled = true;
    return;
  }
  _intervalMs    = (uint32_t)(cfg["battery"]["interval_s"] | 60) * 1000;
  _lowPct        = cfg["battery"]["low_pct"] | 20;

  char msg[64];
  snprintf(msg, sizeof(msg), "Ready - every %lus, low alert at %d%%",
           (unsigned long)(_intervalMs / 1000), _lowPct);
  Log::info(TAG, msg);

  // Register under the unified `sensors` CLI (#126). Use `sensors battery`.
  SensorRegistry::add("battery", "voltage, percent, charging state (PMU)",
    [](ShellOutput out, void* /*ctx*/) {
      if (!PowerManager::isPmuOk())         { out("  PMU not available"); return; }
      if (!PowerManager::isBatteryPresent()) { out("  not detected"); return; }
      char line[64];
      snprintf(line, sizeof(line), "  voltage:  %.2f V",    PowerManager::getVoltage());
      out(line);
      snprintf(line, sizeof(line), "  percent:  %d %%",     PowerManager::getPercent());
      out(line);
      snprintf(line, sizeof(line), "  state:    %s",
               PowerManager::isCharging() ? "charging" : "discharging");
      out(line);
    }, nullptr, true);

  // Publish initial state immediately on boot.
  readAndPublish();
}

// Periodically read and publish battery status at the configured interval
void BatteryModule::loop() {
  if (_disabled || !PowerManager::isPmuOk()) return;

  uint32_t now = millis();
  if (now - _lastRead >= _intervalMs) {
    _lastRead = now;
    readAndPublish();
  }
}

// Read battery voltage/percent/charging state and publish via MQTT and EventBus
void BatteryModule::readAndPublish() {
  float voltage  = PowerManager::getVoltage();
  int   percent  = PowerManager::getPercent();
  bool  charging = PowerManager::isCharging();
  bool  present  = PowerManager::isBatteryPresent();

  JsonDocument doc;
  doc["present"]    = present;
  doc["voltage_v"]  = present ? (float)(roundf(voltage * 100.0f) / 100.0f) : 0.0f;
  doc["percent"]    = present ? percent : -1;
  doc["charging"]   = charging;

  // MQTT publish
  JsonObject cfg     = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";

  // Per-sensor topics for HA discovery
  if (present) {
    char perTopic[96], val[16];
    snprintf(perTopic, sizeof(perTopic), "%s/sensor/battery/percent", prefix);
    snprintf(val, sizeof(val), "%d", percent);
    MQTTClient::publish(perTopic, val);

    snprintf(perTopic, sizeof(perTopic), "%s/sensor/battery/voltage", prefix);
    snprintf(val, sizeof(val), "%.2f", voltage);
    MQTTClient::publish(perTopic, val);

    snprintf(perTopic, sizeof(perTopic), "%s/sensor/battery/charging", prefix);
    MQTTClient::publish(perTopic, charging ? "Charging" : "Discharging");
  }

  // Combined JSON topic (for Lua EventBus, backwards compat)
  char topic[64];
  snprintf(topic, sizeof(topic), "%s/sensor/battery", prefix);
  char payload[128];
  serializeJson(doc, payload, sizeof(payload));
  MQTTClient::publish(topic, payload);

  // EventBus - lets TelegramModule fire low battery alerts
  EventBus::publish("battery", doc.as<JsonObject>());

  // Log summary
  if (present) {
    char log[64];
    snprintf(log, sizeof(log), "%.2fV  %d%%  %s",
             voltage, percent, charging ? "charging" : "discharging");
    Log::info(TAG, log);
  } else {
    Log::warn(TAG, "Battery not detected");
  }
}

// Report battery status for module.status command
void BatteryModule::status(ShellOutput out) {
  char line[96];
  snprintf(line, sizeof(line), "pmu=%s  present=%s  %.2fV %d%% %s",
           PowerManager::isPmuOk() ? "ok" : "fail",
           PowerManager::isBatteryPresent() ? "yes" : "no",
           PowerManager::getVoltage(), PowerManager::getPercent(),
           PowerManager::isCharging() ? "CHG" : "DSG");
  out(line);
}

// Run battery self-test checks
void BatteryModule::selftest(ShellOutput out) {
  if (!PowerManager::isPmuOk()) { out("[WARN] PMU not available"); return; }
  if (PowerManager::isBatteryPresent()) {
    char line[64];
    snprintf(line, sizeof(line), "[PASS] Battery: %.2fV %d%% %s",
             PowerManager::getVoltage(), PowerManager::getPercent(),
             PowerManager::isCharging() ? "charging" : "discharging");
    out(line);
  } else {
    out("[WARN] Battery not detected");
  }
}

#ifdef ENABLE_BATTERY
MODULE_REGISTER(BatteryModule, PRIORITY_SENSOR)
#endif
