// thesada-fw - event_bus_table.h
// EventBus subscriber table: named events, callbacks tagged with an owner so
// a Lua reload drops its own and nothing else. No Arduino, host-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <map>
#include <string>
#include <utility>
#include <vector>

enum : uint8_t { EVENT_OWNER_CORE = 0, EVENT_OWNER_LUA = 1 };

// Callbacks added during one publish that still run in it (a reload re-subscribes).
constexpr size_t EVENT_PUBLISH_MARGIN = 64;
// Live callbacks one event holds.
constexpr size_t EVENT_SUBS_MAX = 128;

template <typename CB>
class EventBusTable {
 public:
  // out: false when the event already holds EVENT_SUBS_MAX live callbacks.
  bool subscribe(const std::string& event, CB cb, uint8_t owner = EVENT_OWNER_CORE) {
    std::vector<Entry>& v = _subs[event];
    size_t live = 0;
    for (const Entry& e : v) live += e.live ? 1 : 0;
    if (live >= EVENT_SUBS_MAX) return false;
    v.push_back(Entry{std::move(cb), owner, true});
    return true;
  }

  // Calls every live entry. A callback may subscribe or remove an owner:
  // each call runs on a copy, dropped entries are swept once the outermost
  // publish returns, and entries past start + margin wait for the next one.
  template <typename Arg>
  void publish(const std::string& event, Arg arg) {
    auto it = _subs.find(event);
    if (it == _subs.end()) return;
    std::vector<Entry>& v = it->second;
    const size_t limit = v.size() + EVENT_PUBLISH_MARGIN;
    _depth++;
    for (size_t i = 0; i < v.size() && i < limit; i++) {
      if (!v[i].live) continue;
      CB cb = v[i].cb;
      cb(arg);
    }
    _depth--;
    if (_depth == 0 && _dirty) sweep();
  }

  // in: owner. out: entries dropped.
  size_t removeOwner(uint8_t owner) {
    size_t n = 0;
    for (auto& kv : _subs) {
      for (Entry& e : kv.second) {
        if (!e.live || e.owner != owner) continue;
        e.live = false;
        n++;
      }
    }
    if (n) _dirty = true;
    if (_depth == 0 && _dirty) sweep();
    return n;
  }

  // Entries held, dropped ones not yet swept included.
  size_t size() const {
    size_t n = 0;
    for (const auto& kv : _subs) n += kv.second.size();
    return n;
  }

 private:
  struct Entry {
    CB      cb;
    uint8_t owner;
    bool    live;
  };

  void sweep() {
    for (auto& kv : _subs) {
      std::vector<Entry>& v = kv.second;
      size_t w = 0;
      for (size_t r = 0; r < v.size(); r++) {
        if (v[r].live) v[w++] = v[r];
      }
      v.resize(w);
    }
    _dirty = false;
  }

  std::map<std::string, std::vector<Entry>> _subs;
  unsigned _depth = 0;
  bool     _dirty = false;
};
