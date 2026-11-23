// thesada-fw - EventBus.h
// Lightweight pub/sub for inter-module communication
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <ArduinoJson.h>
#include <functional>
#include <string>
#include "event_bus_table.h"

using EventCallback = std::function<void(JsonObject)>;

// Single-task invariant: both subscribe() and publish() touch the shared
// _subscribers table with no locking. Every caller must run on the main
// loop task. Do NOT call either from an ISR or a secondary FreeRTOS task -
// concurrent map access corrupts it. If a second task ever needs the bus,
// add a mutex here first.
class EventBus {
public:
  // out: false when the event holds EVENT_SUBS_MAX callbacks already.
  static bool   subscribe(const std::string& event, EventCallback cb,
                          uint8_t owner = EVENT_OWNER_CORE);
  static void   publish(const std::string& event, JsonObject data);
  static size_t unsubscribeOwner(uint8_t owner);
private:
  static EventBusTable<EventCallback> _subscribers;
};
