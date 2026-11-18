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
#include <new>
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

// Where a registration's callback box lives. alloc() returns nullptr when the
// heap is out, never throws. Tests swap in one that fails on demand.
struct MqttSubHeap {
  static void* alloc(size_t n) { return ::operator new(n, std::nothrow); }
  static void  release(void* p) { ::operator delete(p); }
};

// CB is the callback type - std::function on the device, a plain function
// pointer in the host tests - so this header pulls in no Arduino headers.
template <typename CB, uint8_t N, size_t TOPIC_CAP = 160, typename Heap = MqttSubHeap>
class MqttSubTable {
  // A registration's one target, held by its slot and by each dispatch
  // running it. Freed when the last of them lets go.
  struct Box {
    CB       fn;
    uint16_t refs;
  };
  static Box* hold(Box* b) {
    b->refs++;
    return b;
  }
  static void drop(Box* b) {
    if (!b || --b->refs) return;
    b->~Box();
    Heap::release(b);
  }

 public:
  struct Slot {
    char topic[TOPIC_CAP];
    Box*    callback;
    bool    active;
    uint8_t owner;
  };

  MqttSubTable() : _slots(), _count(0) {}
  MqttSubTable(const MqttSubTable&) = delete;
  MqttSubTable& operator=(const MqttSubTable&) = delete;
  ~MqttSubTable() {
    for (Slot& s : _slots) drop(s.callback);
  }

  // Drop every registration. Clearing `active` without the count leaves the
  // freed slots skipped by dispatch and makes add() start past them.
  void reset() {
    for (uint8_t i = 0; i < N; i++) _slots[i].active = false;
    _count = 0;
  }

  // false when the table is full, the topic is empty or does not fit a slot,
  // or the heap has no room for the callback. Each leaves the table as it was.
  // A clipped filter resubscribes a different topic. The caller logs.
  // A slot freed by removeOwner() is taken before the count grows.
  bool add(const char* topic, CB cb, uint8_t owner = MQTT_SUB_OWNER_CORE) {
    if (!topic || !*topic || strnlen(topic, TOPIC_CAP) >= TOPIC_CAP) return false;
    uint8_t i = 0;
    while (i < _count && _slots[i].active) i++;
    if (i >= N) return false;
    void* mem = Heap::alloc(sizeof(Box));
    if (!mem) return false;
    Slot& s = _slots[i];
    drop(s.callback);
    s.callback = new (mem) Box{std::move(cb), 1};
    memcpy(s.topic, topic, strlen(topic) + 1);
    s.active               = true;
    s.owner                = owner;
    if (i == _count) _count++;
    return true;
  }

  // Free every slot owner registered. fn(topic) runs once per freed topic no
  // other active slot still holds, for the broker unsubscribe.
  // in: owner, fn (may be null). out: slots freed.
  // Slots are only marked free, never moved, so a dispatch that runs a
  // reload keeps walking a stable table.
  template <typename Fn>
  uint8_t removeOwner(uint8_t owner, Fn fn) {
    uint8_t freed = 0;
    for (uint8_t i = 0; i < _count; i++) {
      if (!_slots[i].active || _slots[i].owner != owner) continue;
      _slots[i].active = false;
      freed++;
    }
    for (uint8_t i = 0; fn && freed && i < _count; i++) {
      const Slot& s = _slots[i];
      if (s.active || s.owner != owner || isRegistered(s.topic)) continue;
      bool seen = false;
      for (uint8_t j = 0; j < i && !seen; j++) {
        seen = !_slots[j].active && _slots[j].owner == owner && strcmp(_slots[j].topic, s.topic) == 0;
      }
      if (!seen) fn(s.topic);
    }
    while (_count > 0 && !_slots[_count - 1].active) _count--;
    return freed;
  }
  uint8_t removeOwner(uint8_t owner) { return removeOwner(owner, (void (*)(const char*)) nullptr); }

  uint8_t                  count() const { return _count; }
  // in: none. out: true when add() has a slot to take, so a refusal was the heap.
  bool hasFreeSlot() const {
    for (uint8_t i = 0; i < _count; i++) {
      if (!_slots[i].active) return true;
    }
    return _count < N;
  }
  static constexpr uint8_t capacity() { return N; }

  bool isRegistered(const char* topic) const {
    for (uint8_t i = 0; i < _count; i++) {
      if (_slots[i].active && strcmp(_slots[i].topic, topic) == 0) return true;
    }
    return false;
  }

  // Deliver to every active slot whose filter matches. Each call holds a
  // handle to its target: an add() inside it may reuse the slot.
  void dispatch(const char* topic, const char* payload) const {
    for (uint8_t i = 0; i < _count; i++) {
      if (!_slots[i].active || !mqttSubMatches(_slots[i].topic, topic)) continue;
      Box* b = hold(_slots[i].callback);
      b->fn(topic, payload);
      // A box still in its slot keeps the slot's reference, so only a replaced one can go.
      if (_slots[i].callback == b) {
        b->refs--;
      } else {
        drop(b);
      }
    }
  }

  // Active topics, in registration order. Used to resubscribe after a
  // reconnect.
  template <typename Fn>
  void forEachActive(Fn fn) const {
    for (uint8_t i = 0; i < _count; i++) {
      if (_slots[i].active) fn(_slots[i].topic);
    }
  }

 private:
  Slot    _slots[N];
  uint8_t _count;
};
