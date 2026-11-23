// thesada-fw - EventBus.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "EventBus.h"

EventBusTable<EventCallback> EventBus::_subscribers;

// Register a callback for a named event, tagged with its owner
bool EventBus::subscribe(const std::string& event, EventCallback cb, uint8_t owner) {
  return _subscribers.subscribe(event, cb, owner);
}

// Notify all subscribers of an event with optional data
void EventBus::publish(const std::string& event, JsonObject data) {
  _subscribers.publish(event, data);
}

// Drop every callback owner registered. out: callbacks dropped.
size_t EventBus::unsubscribeOwner(uint8_t owner) {
  return _subscribers.removeOwner(owner);
}
