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
#include <Shell.h>
#include <MQTTClient.h>

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

// Register the MQTTClient publish forwarder so every WiFi-disconnect
// publish routes through Cellular::publish when this module is ACTIVE.
// Single canonical publish path - source modules just call
// MQTTClient::publish and the right transport carries it.
//
// In:  none
// Out: STANDBY state, MQTTClient publish forwarder installed.
void CellularModule::begin() {
  MQTTClient::setPublishForwarder([](const char* topic, const char* payload, bool retain) -> bool {
    if (!Cellular::connected()) return false;
    return Cellular::publish(topic, payload, retain);
  });

  // Cellular bring-up subscribes a root wildcard per top-level prefix
  // (smsubAll). Runtime additions to MQTTClient::_subs are already
  // covered by that wildcard, so no per-topic forwarder is needed -
  // installing one would push the modem past its ~4-subscription cap
  // and silently break URC delivery.
  // MQTTClient::setSubscribeForwarder(nullptr);  // explicit, but default is null

  Shell::registerCommand("cell.at",
    "Send raw AT command (cell.at <cmd>; e.g. cell.at +CSQ, cell.at +COPS=?)",
    [](int argc, char** argv, ShellOutput out) {
      if (argc < 2) { out("usage: cell.at <command-without-AT-prefix>"); return; }
      String cmd;
      for (int i = 1; i < argc; ++i) {
        if (i > 1) cmd += " ";
        cmd += argv[i];
      }
      uint32_t timeout = 5000;
      // Operator scan can take a long time on cold radio.
      if (cmd.indexOf("+COPS=?") >= 0) timeout = 180000;
      Cellular::atPassthrough(cmd.c_str(), timeout, out);
    });

  Shell::registerCommand("cell.reset",
    "Hardware-reset modem (DC3+BLDO2 cycle + PWRKEY) and re-walk bring-up",
    [](int argc, char** argv, ShellOutput out) {
      out("Hardware-resetting modem; re-registration will follow on next loop");
      bool ok = Cellular::hardReset();
      out(ok ? "Modem back after hardware reset" : "Modem hardware-reset timeout");
    });
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
        // Tell MQTTClient to stop enqueueing during the WiFi outage; cellular
        // is now shipping the same data via EventBus.
        MQTTClient::setFallbackPublishing(true);
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
      // Cellular::hardReset() (shell or watchdog) drops _started back to
      // false. Detect that and re-walk bring-up; Cellular::loop is a
      // no-op while _started=false and would otherwise leave us stuck.
      if (!Cellular::isModemAlive()) {
        Log::warn(TAG, "Modem hard-reset detected - re-walking activation");
        MQTTClient::setFallbackPublishing(false);
        _state           = State::ACTIVATING;
        _lastTelemetryMs = 0;
        break;
      }

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
          MQTTClient::setFallbackPublishing(false);
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

// Retired: the per-event bridges (temperature/current/battery/alert
// -> Cellular::publish) used to forward the same payloads each source
// module already publishes through MQTTClient::publish. With the publish
// forwarder installed in begin(), MQTTClient::publish routes directly to
// Cellular::publish on a WiFi disconnect, so this duplicate path is gone.
void CellularModule::subscribeEvents() {}

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
