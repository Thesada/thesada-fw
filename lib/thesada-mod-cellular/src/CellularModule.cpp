// thesada-fw - CellularModule.cpp
// Owns the network-selection policy: when to bring the SIM7080G cellular
// path up and when to yield back to WiFi. Hysteresis on both edges
// (60s) so a brief WiFi flap does not thrash the modem and a brief
// WiFi recovery does not yank a working cellular session.
//
// Lifecycle:
//   STANDBY    - WiFi up (or unknown). Cellular not started.
//   ACTIVATING - WiFi has been down >= WIFI_DOWN_HOLD_MS. Bring modem up.
//   ACTIVE     - Cellular link offered to MQTTClient as its transport.
//                Watching for WiFi recovery. On WiFi up >= WIFI_UP_HOLD_MS,
//                withdraw it and drop back to STANDBY (modem stays
//                registered for fast re-takeover).
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
#include <Net.h>
#include <ctime>
#include <cstring>
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

// Register the cell.* shell commands and the Net provider that hands the
// core its modem link (DNS, NTP, HTTPS, the MQTT socket).
//
// In:  none
// Out: STANDBY state, Net::CellularProvider installed.
void CellularModule::begin() {
  Shell::registerCommand("cell.at",
    "Send raw AT command (cell.at <cmd>; e.g. cell.at +CSQ, cell.at +COPS=?)",
    [](int argc, char** argv, ShellOutput out) {
      if (argc < 2) { out("usage: cell.at <command-without-AT-prefix>"); return; }
      String cmd(argv[1]);  // restFrom 1: the AT command as typed
      uint32_t timeout = 5000;
      // Operator scan can take a long time on cold radio.
      if (cmd.indexOf("+COPS=?") >= 0) timeout = 180000;
      Cellular::atPassthrough(cmd.c_str(), timeout, out);
    }, 1);

  Shell::registerCommand("cell.reset",
    "Hardware-reset modem (DC3+BLDO2 cycle + PWRKEY) and re-walk bring-up",
    [](int argc, char** argv, ShellOutput out) {
      out("Hardware-resetting modem; re-registration will follow on next loop");
      bool ok = Cellular::hardReset();
      out(ok ? "Modem back after hardware reset" : "Modem hardware-reset timeout");
    });

  // No standalone cell.http command - the modem HTTPS path is reached
  // through net.http, which auto-routes WiFi-vs-cellular to
  // Cellular::httpsGet via the Net provider hook below, as OTA does.

  // Wire the Net transport-abstraction hook so thesada-core's net.* shell
  // commands (net.ip / net.ping / net.ntp / net.http) keep working on the
  // cellular leg when WiFi is down. Captureless lambdas decay to plain
  // function pointers - the provider table stays trivially copyable.
  Net::CellularProvider provider = {};
  provider.linkUp   = []() { return Cellular::dataLinkUp(); };
  provider.linkInfo = [](std::function<void(const char*)> emit) {
    Cellular::netInfo(emit);
  };
  provider.resolve  = [](const char* host, char* out, size_t outLen) {
    return Cellular::resolveHost(host, out, outLen);
  };
  provider.ntpSync  = [](const char* server, uint32_t timeoutMs) {
    return Cellular::ntpSync(server, timeoutMs);
  };
  provider.httpsGet = [](const char* host, const char* path, uint16_t port,
                         const char* caPem,
                         std::function<bool(const uint8_t*, size_t)> onBody,
                         int* httpStatus) {
    return Cellular::httpsGet(host, path, port, caPem, onBody, httpStatus);
  };
  provider.tcp = [](uint8_t slot) { return Cellular::tcpClient(slot); };
  Net::setCellularProvider(provider);

  Log::info(TAG, "cellular.module_ready state=standby");
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
          Log::kvfw(TAG, "cellular.state_change from=standby to=activating reason=wifi_down_60s");
          _state = State::ACTIVATING;
        }
      } else {
        _wifiDownSince = 0;
      }
      break;
    }

    case State::ACTIVATING: {
      // tickActivation() advances one bring-up phase per call and returns
      // fast, so every other module loop keeps ticking while the modem
      // registers. Stay in ACTIVATING until it reaches DONE or FAILED.
      switch (Cellular::tickActivation()) {
        case Cellular::ActStatus::PENDING:
          // Bring-up still walking its phases - re-poll next loop.
          break;

        case Cellular::ActStatus::DONE: {
          Log::kvf(TAG, "cellular.state_change from=activating to=active");
          _state           = State::ACTIVE;
          _wifiUpSince     = 0;
          _lastTelemetryMs = 0;

          // Clock recovery: when WiFi never associated, the SNTP client
          // in WiFiManager never ran and the system clock is stuck
          // pre-2023. Sync it off the modem now that the data context is
          // up, so log timestamps and TLS validity windows are correct
          // on a cellular-only boot.
          if (time(nullptr) < 1700000000L) {
            JsonObject  ncfg = Config::get();
            const char* nsrv = ncfg["ntp"]["server"]         | "pool.ntp.org";
            uint32_t    nto  = (ncfg["ntp"]["cell_timeout_s"] | 60) * 1000UL;
            if (Cellular::ntpSync(nsrv, nto)) {
              Log::info(TAG, "cellular.ntp_synced");
            } else {
              Log::warn(TAG, "cellular.ntp_sync_failed clock=unset");
            }
          }

          // After the clock fix: certificate checks want a sane clock.
          // The session's connect republishes the retained set.
          MQTTClient::setCellularTransport(true);
          break;
        }

        case Cellular::ActStatus::FAILED:
          // Hard failure or the activation deadline expired. Drop back to
          // STANDBY and reset the down-hold so we do not retry on every
          // loop tick - the next 60 s WiFi-down hold re-arms activation.
          Log::kvfw(TAG, "cellular.state_change from=activating to=standby reason=activation_failed");
          _wifiDownSince = now;
          _state         = State::STANDBY;
          break;
      }
      break;
    }

    case State::ACTIVE: {
      // Cellular::hardReset() (shell or watchdog) drops _started back to
      // false. Detect that and re-walk bring-up; Cellular::loop is a
      // no-op while _started=false and would otherwise leave us stuck.
      // A hard reset that succeeds leaves the modem alive but not
      // activated, so both are checked.
      if (!Cellular::isModemAlive() || !Cellular::activated()) {
        Log::kvfw(TAG, "cellular.state_change from=active to=activating reason=modem_hard_reset");
        MQTTClient::setCellularTransport(false);
        _state           = State::ACTIVATING;
        _lastTelemetryMs = 0;
        break;
      }

      // Run cellular-side recovery (re-register, re-open the bearer).
      Cellular::loop();

      if (wifiUp) {
        if (_wifiUpSince == 0) _wifiUpSince = now;
        if (now - _wifiUpSince >= WIFI_UP_HOLD_MS) {
          Log::kvf(TAG, "cellular.state_change from=active to=standby reason=wifi_up_60s");
          // The session is already back on WiFi (it moves as soon as WiFi
          // is up). Re-takeover from a future WiFi drop is fast while the
          // modem stays registered.
          emitActive(0);
          MQTTClient::setCellularTransport(false);
          // link_mode "fallback" (default): tear down the data context
          // now WiFi is back so an idle PDP context does not burn
          // metered data/battery. "standby" leaves it warm for instant
          // re-takeover. Modem stays network-registered either way.
          {
            JsonObject  cfg      = Config::get();
            const char* linkMode = cfg["cellular"]["link_mode"] | "fallback";
            if (strcmp(linkMode, "standby") != 0) Cellular::dataLinkDown();
          }
          _state         = State::STANDBY;
          _wifiDownSince = 0;
          _wifiUpSince   = 0;
        }
      } else {
        _wifiUpSince = 0;
      }

      // The first sample doubles as the "active" announcement, once the
      // session is up on the modem.
      if (MQTTClient::connected() && now - _lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
        if (_lastTelemetryMs == 0) emitActive(1);
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
// Out: MQTT publish (best-effort)
void CellularModule::emitActive(int active) {
  JsonObject  cfg = Config::get();
  const char* pfx = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  char topic[MQTT_TOPIC_CAP];
  snprintf(topic, sizeof(topic), "%s/cellular/active", pfx);
  MQTTClient::publish(topic, active ? "1" : "0");
}

// ---------------------------------------------------------------------------

// Publish <prefix>/cellular/rssi with the current AT+CSQ value.
// 0..31 valid, 99 means unknown - emitted anyway so the dashboard can
// distinguish "active but no signal sample yet" from "not active".
//
// In:  none (reads Cellular::getSignalQuality())
// Out: MQTT publish (best-effort)
void CellularModule::emitRssi() {
  JsonObject  cfg = Config::get();
  const char* pfx = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  char topic[MQTT_TOPIC_CAP], val[8];
  snprintf(topic, sizeof(topic), "%s/cellular/rssi", pfx);
  snprintf(val, sizeof(val), "%d", Cellular::getSignalQuality());
  MQTTClient::publish(topic, val);
}

// ---------------------------------------------------------------------------

// No per-event bridges: every module publishes through MQTTClient, which
// carries the session on whichever transport is live.
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
  snprintf(line, sizeof(line), "state=%s link=%d rssi=%d mqtt=%s",
           s, (int)Cellular::connected(), Cellular::getSignalQuality(),
           MQTTClient::transportName());
  out(line);
}

MODULE_REGISTER(CellularModule, PRIORITY_NETWORK)

#endif // ENABLE_CELLULAR
