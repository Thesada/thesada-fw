// thesada-fw - mqtt_sub_table.h
// Pure MQTT subscription table: fixed slots, registration, and the
// exact / trailing-# / trailing-+ topic match. No Arduino, host-testable.
//
// The invariant this type exists to hold: slot states and the slot count are
// only ever cleared together. Clearing one and not the other stranded the
// OTA command topic in slot 0 for a whole boot - see docs/invariants.md.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <utility>

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

// Who registered a slot. Core slots are rebuilt by reinitSubscriptions(),
// Lua slots are freed by a script reload and kept across a reinit.
enum : uint8_t { MQTT_SUB_OWNER_CORE = 0, MQTT_SUB_OWNER_LUA = 1 };

// A slot removed during a dispatch turns ZOMBIE until the outermost one returns.
enum : uint8_t { MQTT_SLOT_FREE = 0, MQTT_SLOT_ACTIVE = 1, MQTT_SLOT_ZOMBIE = 2 };

// CB is std::function on the device, a function pointer in the host tests. Held
// inline, no heap: captures stay trivially copyable and within 16 bytes.
template <typename CB, uint8_t N, size_t TOPIC_CAP = 160>
class MqttSubTable {
 public:
  struct Slot {
    char    topic[TOPIC_CAP];
    CB      fn;
    uint8_t state;
    uint8_t owner;
  };

  MqttSubTable() : _slots(), _count(0), _depth(0) {}
  MqttSubTable(const MqttSubTable&) = delete;
  MqttSubTable& operator=(const MqttSubTable&) = delete;

  // Drop every registration. Clearing the states without the count leaves the
  // freed slots skipped by dispatch and makes add() start past them.
  void reset() {
    for (uint8_t i = 0; i < _count; i++) {
      if (_slots[i].state == MQTT_SLOT_ACTIVE) retire(_slots[i]);
    }
    shrink();
  }

  // false when the table is full, or the topic is empty or does not fit a
  // slot. Each leaves the table as it was. A clipped filter resubscribes a
  // different topic. The caller logs. A FREE slot below the count is taken
  // before the count grows, a ZOMBIE never.
  bool add(const char* topic, CB cb, uint8_t owner = MQTT_SUB_OWNER_CORE) {
    if (!topic || !*topic || strnlen(topic, TOPIC_CAP) >= TOPIC_CAP) return false;
    uint8_t i = 0;
    while (i < _count && _slots[i].state != MQTT_SLOT_FREE) i++;
    if (i >= N) return false;
    Slot& s = _slots[i];
    s.fn    = std::move(cb);
    memcpy(s.topic, topic, strlen(topic) + 1);
    s.state = MQTT_SLOT_ACTIVE;
    s.owner = owner;
    if (i == _count) _count++;
    return true;
  }

  // Free every slot owner registered. fn(topic) runs once per freed topic no
  // other active slot still holds, for the broker unsubscribe.
  // in: owner, fn (may be null). out: slots freed.
  // Slots are only marked, never moved, so a dispatch that runs a reload
  // keeps walking a stable table.
  template <typename Fn>
  uint8_t removeOwner(uint8_t owner, Fn fn) {
    for (uint8_t i = 0; fn && i < _count; i++) {
      const Slot& s = _slots[i];
      if (!removes(s, owner) || heldByOther(s.topic, owner)) continue;
      bool seen = false;
      for (uint8_t j = 0; j < i && !seen; j++) {
        seen = removes(_slots[j], owner) && strcmp(_slots[j].topic, s.topic) == 0;
      }
      if (!seen) fn(s.topic);
    }
    uint8_t freed = 0;
    for (uint8_t i = 0; i < _count; i++) {
      if (!removes(_slots[i], owner)) continue;
      retire(_slots[i]);
      freed++;
    }
    shrink();
    return freed;
  }
  uint8_t removeOwner(uint8_t owner) { return removeOwner(owner, (void (*)(const char*)) nullptr); }

  // Slots in use, zombies included until the outermost dispatch returns.
  uint8_t                  count() const { return _count; }
  static constexpr uint8_t capacity() { return N; }

  bool isRegistered(const char* topic) const {
    for (uint8_t i = 0; i < _count; i++) {
      if (_slots[i].state == MQTT_SLOT_ACTIVE && strcmp(_slots[i].topic, topic) == 0) return true;
    }
    return false;
  }

  // Deliver to every active slot whose filter matches, calling each callback
  // in its slot. Slots added during the call past the starting count wait for
  // the next message.
  void dispatch(const char* topic, const char* payload) {
    const uint8_t n = _count;
    _depth++;
    for (uint8_t i = 0; i < n; i++) {
      Slot& s = _slots[i];
      if (s.state == MQTT_SLOT_ACTIVE && mqttSubMatches(s.topic, topic)) s.fn(topic, payload);
    }
    _depth--;
    if (_depth == 0) sweep();
  }

  // Active topics, in registration order. Used to resubscribe after a
  // reconnect.
  template <typename Fn>
  void forEachActive(Fn fn) const {
    for (uint8_t i = 0; i < _count; i++) {
      if (_slots[i].state == MQTT_SLOT_ACTIVE) fn(_slots[i].topic);
    }
  }

 private:
  static bool removes(const Slot& s, uint8_t owner) {
    return s.state == MQTT_SLOT_ACTIVE && s.owner == owner;
  }
  bool heldByOther(const char* topic, uint8_t owner) const {
    for (uint8_t i = 0; i < _count; i++) {
      const Slot& s = _slots[i];
      if (s.state == MQTT_SLOT_ACTIVE && s.owner != owner && strcmp(s.topic, topic) == 0) return true;
    }
    return false;
  }
  // ACTIVE to ZOMBIE inside a dispatch, to FREE outside one.
  void retire(Slot& s) {
    if (_depth > 0) {
      s.state = MQTT_SLOT_ZOMBIE;
      return;
    }
    s.state = MQTT_SLOT_FREE;
    s.fn    = CB();
  }
  void sweep() {
    for (uint8_t i = 0; i < _count; i++) {
      if (_slots[i].state != MQTT_SLOT_ZOMBIE) continue;
      _slots[i].state = MQTT_SLOT_FREE;
      _slots[i].fn    = CB();
    }
    shrink();
  }
  void shrink() {
    while (_count > 0 && _slots[_count - 1].state == MQTT_SLOT_FREE) _count--;
  }

  Slot     _slots[N];
  uint8_t  _count;
  unsigned _depth;
};
