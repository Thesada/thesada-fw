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
#include <Net.h>
#include <ctime>
#include <cstring>
#include <esp_task_wdt.h>
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

  // cell.cert.test / cell.cert.dump / cell.smconn.test expose modem-internal
  // cert state and raw handshake probes through the shell. Useful on the
  // bench when chasing mTLS / modem-cert upload bugs, pure noise and extra
  // attack surface on a deployed OWB - gated to debug builds only.
#ifdef THESADA_CELL_DEBUG
  // cell.cert.test: read back the size of the modem-side client.crt
  // (filled by mqttConnect's writeClientCert) and try CSSLCFG CONVERT
  // with several ssltype slots. Lets us pin down whether mTLS failures
  // are upload-side (file empty / not on FS) or convert-side (wrong
  // ssltype enum for this SIM7080 firmware revision) without waiting
  // for the next backoff window.
  Shell::registerCommand("cell.cert.test",
    "Probe modem-side client.crt: file size + CSSLCFG CONVERT slot tests",
    [](int argc, char** argv, ShellOutput out) {
      out("--- CFSINIT ---");
      Cellular::atPassthrough("+CFSINIT", 5000, out);
      out("--- CFSGFIS client.crt (size) ---");
      Cellular::atPassthrough("+CFSGFIS=3,\"client.crt\"", 5000, out);
      out("--- CFSGFIS server-ca.crt (size, control) ---");
      Cellular::atPassthrough("+CFSGFIS=3,\"server-ca.crt\"", 5000, out);
      out("--- CFSTERM ---");
      Cellular::atPassthrough("+CFSTERM", 5000, out);
      out("--- CONVERT 2 server-ca.crt (control - known good) ---");
      Cellular::atPassthrough("+CSSLCFG=\"CONVERT\",2,\"server-ca.crt\"", 5000, out);
      out("--- CONVERT 1 client.crt (current code path) ---");
      Cellular::atPassthrough("+CSSLCFG=\"CONVERT\",1,\"client.crt\"", 5000, out);
      out("--- CONVERT 2 client.crt (alt - if fw enum differs) ---");
      Cellular::atPassthrough("+CSSLCFG=\"CONVERT\",2,\"client.crt\"", 5000, out);
      out("--- CONVERT 3 client.crt (alt - some fw uses type 3) ---");
      Cellular::atPassthrough("+CSSLCFG=\"CONVERT\",3,\"client.crt\"", 5000, out);
      out("--- done ---");
    });

  // cell.smconn.test: tear down + reconnect cycle that lets us see the
  // SIM7080's real handshake error. Existing mqttConnect() drainModemTail
  // misses post-ERROR chatter on slow handshakes; this command issues
  // SMDISC, configures SMSSL=2 mTLS, runs SMCONN, then idles 5 s and
  // dumps everything Serial1 emits in that window. Run while cellular
  // is in a Retry MQTT backoff (the AT bus pause now releases it).
  // cell.cert.dump: read modem-side client.crt back as ASCII. CFSRFILE
  // appears to need CFSINIT + CFSRFILE + CFSTERM in one continuous AT
  // session on this fw rev; running them via three separate cell.at
  // shell calls (each its own ATGuard) returns ERROR. Issuing them
  // back-to-back inside one atPassthrough chain via the same shell
  // command works.
  Shell::registerCommand("cell.cert.dump",
    "Read modem-side client.crt content (CFSINIT/CFSRFILE/CFSTERM in one chain)",
    [](int argc, char** argv, ShellOutput out) {
      out("--- CFSINIT ---");
      Cellular::atPassthrough("+CFSINIT", 5000, out);
      out("--- CFSRFILE 1024 B from offset 0 ---");
      Cellular::atPassthrough("+CFSRFILE=3,\"client.crt\",0,1024,0", 8000, out);
      out("--- CFSRFILE next 1024 B from offset 1024 ---");
      Cellular::atPassthrough("+CFSRFILE=3,\"client.crt\",0,1024,1024", 8000, out);
      out("--- CFSTERM ---");
      Cellular::atPassthrough("+CFSTERM", 5000, out);
      out("--- done ---");
    });

  Shell::registerCommand("cell.smconn.test",
    "Manual SMCONN attempt with extended error drain (use during retry)",
    [](int argc, char** argv, ShellOutput out) {
      out("--- SMDISC ---");
      Cellular::atPassthrough("+SMDISC", 5000, out);
      out("--- SMSSL=2 mTLS attach ---");
      Cellular::atPassthrough("+SMSSL=2,\"server-ca.crt\",\"client.crt\"", 5000, out);
      out("--- SMCONN (15s + 5s idle for late chatter) ---");
      Cellular::atPassthrough("+SMCONN", 15000, out);
      // Idle drain - the real cause often arrives after the bare ERROR.
      // Chunk the wait + feed TWDT so the 30s task watchdog stays happy.
      for (int i = 0; i < 10; ++i) { delay(500); esp_task_wdt_reset(); }
      Cellular::atPassthrough("+SMSTATE?", 3000, out);
      out("--- done ---");
    });
#endif // THESADA_CELL_DEBUG

  // No standalone cell.http command - the modem HTTPS path is reached
  // through net.http, which auto-routes WiFi-vs-cellular. Cellular::httpsGet
  // stays as the implementation (used by the net.http cellular branch via
  // the Net provider hook below, and by the OTA cellular fallback).

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
                         std::function<bool(const uint8_t*, size_t)> onBody,
                         int* httpStatus) {
    return Cellular::httpsGet(host, path, port, onBody, httpStatus);
  };
  Net::setCellularProvider(provider);

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
        // Republish retained state (availability "online", HA discovery,
        // /info, retained manifest) over the cellular leg so cellular-only
        // sessions converge to the same broker-side state as WiFi sessions.
        // Session-flag inside publishRetainedSet guards against repeats on
        // STANDBY -> ACTIVE bouncing within one fallback window.
        MQTTClient::publishRetainedSet();

        // Clock recovery: when WiFi never associated, the SNTP client in
        // WiFiManager never ran and the system clock is stuck pre-2023.
        // Sync it off the modem now that the data context is up, so log
        // timestamps and TLS validity windows are correct on a
        // cellular-only boot.
        if (time(nullptr) < 1700000000L) {
          JsonObject  ncfg = Config::get();
          const char* nsrv = ncfg["ntp"]["server"]         | "pool.ntp.org";
          uint32_t    nto  = (ncfg["ntp"]["cell_timeout_s"] | 60) * 1000UL;
          if (Cellular::ntpSync(nsrv, nto)) {
            Log::info(TAG, "Clock synced via cellular NTP");
          } else {
            Log::warn(TAG, "Cellular NTP sync failed - clock still unset");
          }
        }

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
