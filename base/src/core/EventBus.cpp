// thesada-fw — EventBus.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "EventBus.h"

std::map<std::string, std::vector<EventCallback>> EventBus::_subscribers;

void EventBus::subscribe(const std::string& event, EventCallback cb) {
  _subscribers[event].push_back(cb);
}

void EventBus::publish(const std::string& event, JsonObject data) {
  auto it = _subscribers.find(event);
  if (it != _subscribers.end()) {
    for (auto& cb : it->second) cb(data);
  }
}
