// thesada-fw - CellularModule.cpp
// Activates the SIM7080G cellular path when WiFi has completely failed.
// SPDX-License-Identifier: GPL-3.0-only

#include <thesada_config.h>
#ifdef ENABLE_CELLULAR

#include "CellularModule.h"
#include "Cellular.h"
#include <Config.h>
#include <EventBus.h>
#include <Log.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <ModuleRegistry.h>

static const char* TAG = "CellularModule";

// Activate cellular path if WiFi has failed, then subscribe to sensor events
void CellularModule::begin() {
  // Only take over the network path if WiFi could not connect.
  if (WiFiManager::status() != WiFiStatus::ALL_FAILED) {
    Log::info(TAG, "WiFi active - cellular standby");
    return;
  }

  Cellular::begin();
  subscribeEvents();
}

// Delegate to Cellular::loop for WiFi recheck and connection recovery
void CellularModule::loop() {
  // If WiFi came back (recheckWiFi inside Cellular::loop() returns true),
  // Cellular::connected() will become false and we stop publishing from here.
  Cellular::loop();
}

// Subscribe to EventBus sensor events and route them to Cellular::publish
void CellularModule::subscribeEvents() {
  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";

  // Capture prefix by value into lambdas (copy to a static buffer so it
  // outlives the stack frame - Config doc is persistent so pointer is stable).
  // Per-sensor topics (matches WiFi MQTT path)
  EventBus::subscribe("temperature", [](JsonObject data) {
    if (!Cellular::connected()) return;
    JsonObject  cfg = Config::get();
    const char* pfx = cfg["mqtt"]["topic_prefix"] | "thesada/node";
    JsonArray sensors = data["sensors"];
    if (!sensors) return;
    for (JsonObject s : sensors) {
      const char* name = s["name"] | "unknown";
      float temp = s["temp_c"] | -127.0f;
      if (temp <= -126.0f) continue;
      // Slugify name: "House Supply" -> "house_supply"
      char slug[32];
      int j = 0;
      for (int i = 0; name[i] && j < (int)sizeof(slug) - 1; i++) {
        slug[j++] = (name[i] == ' ') ? '_' : tolower(name[i]);
      }
      slug[j] = '\0';
      char topic[96], val[16];
      snprintf(topic, sizeof(topic), "%s/sensor/temperature/%s", pfx, slug);
      snprintf(val, sizeof(val), "%.2f", temp);
      Cellular::publish(topic, val);
    }
  });

  EventBus::subscribe("current", [](JsonObject data) {
    if (!Cellular::connected()) return;
    JsonObject  cfg = Config::get();
    const char* pfx = cfg["mqtt"]["topic_prefix"] | "thesada/node";
    JsonArray channels = data["channels"];
    if (!channels) return;
    for (JsonObject ch : channels) {
      const char* name = ch["name"] | "unknown";
      char slug[32];
      int j = 0;
      for (int i = 0; name[i] && j < (int)sizeof(slug) - 1; i++) {
        slug[j++] = (name[i] == ' ') ? '_' : tolower(name[i]);
      }
      slug[j] = '\0';
      char topic[96], val[16];
      snprintf(topic, sizeof(topic), "%s/sensor/current/%s", pfx, slug);
      snprintf(val, sizeof(val), "%.4f", ch["current_a"] | 0.0f);
      Cellular::publish(topic, val);
      snprintf(topic, sizeof(topic), "%s/sensor/power/%s", pfx, slug);
      snprintf(val, sizeof(val), "%.1f", ch["power_w"] | 0.0f);
      Cellular::publish(topic, val);
    }
  });

  EventBus::subscribe("battery", [](JsonObject data) {
    if (!Cellular::connected()) return;
    JsonObject  cfg = Config::get();
    const char* pfx = cfg["mqtt"]["topic_prefix"] | "thesada/node";
    bool present = data["present"] | false;
    if (!present) return;
    char topic[96], val[16];
    snprintf(topic, sizeof(topic), "%s/sensor/battery/percent", pfx);
    snprintf(val, sizeof(val), "%d", data["percent"] | -1);
    Cellular::publish(topic, val);
    snprintf(topic, sizeof(topic), "%s/sensor/battery/voltage", pfx);
    snprintf(val, sizeof(val), "%.2f", data["voltage_v"] | 0.0f);
    Cellular::publish(topic, val);
    snprintf(topic, sizeof(topic), "%s/sensor/battery/charging", pfx);
    Cellular::publish(topic, (data["charging"] | false) ? "Charging" : "Discharging");
  });

  EventBus::subscribe("alert", [](JsonObject data) {
    if (!Cellular::connected()) return;
    JsonObject  cfg = Config::get();
    const char* pfx = cfg["mqtt"]["topic_prefix"] | "thesada/node";
    char topic[80];
    snprintf(topic, sizeof(topic), "%s/alert", pfx);
    String payload;
    serializeJson(data, payload);
    Cellular::publish(topic, payload.c_str());
  });

  Log::info(TAG, "Subscribed to sensor events");
}

// Report cellular module status
void CellularModule::status(ShellOutput out) {
  out("compiled");
}

MODULE_REGISTER(CellularModule, PRIORITY_NETWORK)

#endif // ENABLE_CELLULAR
