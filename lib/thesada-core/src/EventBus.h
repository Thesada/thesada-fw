// thesada-fw - EventBus.h
// Lightweight pub/sub for inter-module communication
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <ArduinoJson.h>
#include <functional>
#include <map>
#include <vector>
#include <string>

using EventCallback = std::function<void(JsonObject)>;

// Single-task invariant: both subscribe() and publish() touch the shared
// _subscribers map with no locking. Every caller must run on the main
// loop task. Do NOT call either from an ISR or a secondary FreeRTOS task -
// concurrent map access corrupts it. If a second task ever needs the bus,
// add a mutex here first.
class EventBus {
public:
  static void subscribe(const std::string& event, EventCallback cb);
  static void publish(const std::string& event, JsonObject data);
private:
  static std::map<std::string, std::vector<EventCallback>> _subscribers;
};
