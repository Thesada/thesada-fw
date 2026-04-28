// thesada-fw - CellularModule.cpp
// Owns the network-selection policy: when to bring the SIM7080G cellular
// path up and when to yield back to WiFi. Hysteresis on both edges
// (60s) so a brief WiFi flap does not thrash the modem and a brief
// WiFi recovery does not yank a working cellular session.
//
// Lifecycle:
//   STANDBY    - WiFi up (or unknown). Cellular not started.
//   ACTIVATING - WiFi has been down >= WIFI_DOWN_HOLD_MS. Bring modem up.
//   ACTIVE     - Cellular up and publishing. Watching for WiFi recovery.
//                On WiFi up >= WIFI_UP_HOLD_MS, close publish gate and
//                drop back to STANDBY (modem stays connected for fast
//                re-takeover).
//
// Telemetry while ACTIVE:
//   <prefix>/cellular/active = "1" on activation, "0" on yield
//   <prefix>/cellular/rssi   = AT+CSQ value (0..31, 99 = unknown), 60s cadence
//
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

// Hysteresis: required steady WiFi-down before activating cellular,
// and required steady WiFi-up before yielding back. 60s each is short
// enough to recover an outage quickly but long enough to ride out a
// momentary scan or roam.
static constexpr uint32_t WIFI_DOWN_HOLD_MS = 60UL * 1000UL;
static constexpr uint32_t WIFI_UP_HOLD_MS   = 60UL * 1000UL;

// Cellular telemetry cadence while ACTIVE.
static constexpr uint32_t TELEMETRY_INTERVAL_MS = 60UL * 1000UL;

// ── Static state ────────────────────────────────────────────────────────────
CellularModule::State CellularModule::_state           = CellularModule::State::STANDBY;
uint32_t              CellularModule::_wifiDownSince   = 0;
uint32_t              CellularModule::_wifiUpSince     = 0;
uint32_t              CellularModule::_lastTelemetryMs = 0;

// ---------------------------------------------------------------------------

// Subscribe to sensor events at boot regardless of activation state, so
// the publish handlers exist the moment cellular comes up later. Each
// handler is a no-op while Cellular::connected() is false.
//
// In:  none
// Out: STANDBY state, EventBus subscriptions registered.
void CellularModule::begin() {
  subscribeEvents();
  Log::info(TAG, "Cellular module ready (standby)");
}

// ---------------------------------------------------------------------------

// Drive the state machine. Polls WiFiManager::status() every loop, applies
// the hold timers, and brings the cellular path up or yields it back.
//
// In:  none (reads WiFiManager + Cellular global state)
// Out: state transitions, modem activation, telemetry publishes
void CellularModule::loop() {
  bool     wifiUp = (WiFiManager::status() == WiFiStatus::CONNECTED);
  uint32_t now    = millis();

  switch (_state) {
    case State::STANDBY: {
      if (!wifiUp) {
        if (_wifiDownSince == 0) _wifiDownSince = now;
        if (now - _wifiDownSince >= WIFI_DOWN_HOLD_MS) {
          Log::warn(TAG, "WiFi down >60s - activating cellular");
          _state = State::ACTIVATING;
        }
      } else {
        _wifiDownSince = 0;
      }
      break;
    }

    case State::ACTIVATING: {
      // Cellular::begin() is idempotent. If the modem is already started
      // (re-entry after a yield), it returns immediately and we just need
      // to re-open the publish gate.
      Cellular::begin();
      Cellular::setPublishGate(true);

      if (Cellular::connected()) {
        Log::info(TAG, "Cellular ACTIVE");
        _state           = State::ACTIVE;
        _wifiUpSince     = 0;
        _lastTelemetryMs = 0;
        emitActive(1);
      } else {
        // begin() did not bring MQTT up. Drop back to STANDBY and reset
        // the down-hold so we do not retry on every loop tick.
        Log::warn(TAG, "Cellular activation failed - back to standby");
        _wifiDownSince = now;
        _state         = State::STANDBY;
      }
      break;
    }

    case State::ACTIVE: {
      // Run cellular-side recovery (re-register on drop, MQTT reconnect).
      Cellular::loop();

      if (wifiUp) {
        if (_wifiUpSince == 0) _wifiUpSince = now;
        if (now - _wifiUpSince >= WIFI_UP_HOLD_MS) {
          Log::info(TAG, "WiFi up >60s - yielding to WiFi");
          emitActive(0);
          // Close the publish gate but leave the modem registered.
          // Re-takeover from a future WiFi drop only needs to re-open
          // the gate (Cellular::begin() is idempotent).
          Cellular::setPublishGate(false);
          _state         = State::STANDBY;
          _wifiDownSince = 0;
          _wifiUpSince   = 0;
        }
      } else {
        _wifiUpSince = 0;
      }

      if (now - _lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
        _lastTelemetryMs = now;
        emitRssi();
      }
      break;
    }
  }
}

// ---------------------------------------------------------------------------

// Publish <prefix>/cellular/active with "1" or "0" on activation/yield.
//
// In:  active - 1 on activation, 0 on yield
// Out: MQTT publish via Cellular::publish (best-effort)
void CellularModule::emitActive(int active) {
  JsonObject  cfg = Config::get();
  const char* pfx = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  char topic[96];
  snprintf(topic, sizeof(topic), "%s/cellular/active", pfx);
  Cellular::publish(topic, active ? "1" : "0");
}

// ---------------------------------------------------------------------------

// Publish <prefix>/cellular/rssi with the current AT+CSQ value.
// 0..31 valid, 99 means unknown - emitted anyway so the dashboard can
// distinguish "active but no signal sample yet" from "not active".
//
// In:  none (reads Cellular::getSignalQuality())
// Out: MQTT publish via Cellular::publish (best-effort)
void CellularModule::emitRssi() {
  JsonObject  cfg = Config::get();
  const char* pfx = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  char topic[96], val[8];
  snprintf(topic, sizeof(topic), "%s/cellular/rssi", pfx);
  snprintf(val, sizeof(val), "%d", Cellular::getSignalQuality());
  Cellular::publish(topic, val);
}

// ---------------------------------------------------------------------------

// Subscribe to EventBus sensor events and route them to Cellular::publish.
// Subscriptions are unconditional; each lambda short-circuits while
// Cellular::connected() is false (closed gate or modem down).
//
// In:  none (reads Config for topic_prefix)
// Out: EventBus subscriptions registered for temperature/current/battery/alert
void CellularModule::subscribeEvents() {
  // Per-sensor topics (matches WiFi MQTT path).
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

// ---------------------------------------------------------------------------

// Report cellular module status to the shell.
//
// In:  out - shell output sink
// Out: one-line state summary
void CellularModule::status(ShellOutput out) {
  const char* s =
    _state == State::STANDBY    ? "standby"    :
    _state == State::ACTIVATING ? "activating" :
                                  "active";
  char line[96];
  snprintf(line, sizeof(line), "state=%s connected=%d rssi=%d",
           s, (int)Cellular::connected(), Cellular::getSignalQuality());
  out(line);
}

MODULE_REGISTER(CellularModule, PRIORITY_NETWORK)

#endif // ENABLE_CELLULAR
