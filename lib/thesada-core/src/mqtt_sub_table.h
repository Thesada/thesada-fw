// thesada-fw - mqtt_sub_table.h
// Pure MQTT subscription table: fixed slots, registration, and the
// exact / trailing-# / trailing-+ topic match. No Arduino, host-testable.
//
// The invariant this type exists to hold: `active` and the slot count are
// only ever cleared together. Clearing one and not the other stranded the
// OTA command topic in slot 0 for a whole boot - see docs/invariants.md.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// MQTT topic match: exact, trailing `/#` (multi-level), trailing `/+` (one
// level). Lifted verbatim from MQTTClient::matchAndDispatch.
inline bool mqttSubMatches(const char* sub, const char* topic) {
  size_t slen = strlen(sub);
  if (slen >= 2 && sub[slen - 1] == '#' && sub[slen - 2] == '/') {
    return strncmp(sub, topic, slen - 1) == 0;
  }
  if (slen >= 2 && sub[slen - 1] == '+' && sub[slen - 2] == '/') {
    if (strncmp(sub, topic, slen - 1) != 0) return false;
    const char* rest = topic + (slen - 1);
    return *rest != '\0' && strchr(rest, '/') == nullptr;
  }
  return strcmp(sub, topic) == 0;
}

// CB is the callback type - std::function on the device, a plain function
// pointer in the host tests - so this header pulls in no Arduino headers.
template <typename CB, uint8_t N, size_t TOPIC_CAP = 96>
class MqttSubTable {
 public:
  struct Slot {
    char topic[TOPIC_CAP];
    CB   callback;
    bool active;
  };

  // Drop every registration. Clearing `active` without the count leaves the
  // freed slots skipped by dispatch and makes add() start past them.
  void reset() {
    for (uint8_t i = 0; i < N; i++) _slots[i].active = false;
    _count = 0;
  }

  // false when the table is full; the caller logs.
  bool add(const char* topic, CB cb) {
    if (_count >= N) return false;
    Slot& s = _slots[_count];
    strncpy(s.topic, topic, TOPIC_CAP - 1);
    s.topic[TOPIC_CAP - 1] = '\0';
    s.callback             = cb;
    s.active               = true;
    _count++;
    return true;
  }

  uint8_t                  count() const { return _count; }
  static constexpr uint8_t capacity() { return N; }

  bool isRegistered(const char* topic) const {
    for (uint8_t i = 0; i < _count; i++) {
      if (_slots[i].active && strcmp(_slots[i].topic, topic) == 0) return true;
    }
    return false;
  }

  // Deliver to every active slot whose filter matches.
  void dispatch(const char* topic, const char* payload) const {
    for (uint8_t i = 0; i < _count; i++) {
      if (!_slots[i].active) continue;
      if (mqttSubMatches(_slots[i].topic, topic)) _slots[i].callback(topic, payload);
    }
  }

  // Active topics, in registration order. Used to replay AT+SMSUB on the
  // cellular session and to resubscribe after a reconnect.
  template <typename Fn>
  void forEachActive(Fn fn) const {
    for (uint8_t i = 0; i < _count; i++) {
      if (_slots[i].active) fn(_slots[i].topic);
    }
  }

 private:
  Slot    _slots[N] = {};
  uint8_t _count    = 0;
};
