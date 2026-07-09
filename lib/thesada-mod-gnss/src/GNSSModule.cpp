// thesada-fw - GNSSModule.cpp
// SPDX-License-Identifier: GPL-3.0-only

#include <thesada_config.h>
#ifdef ENABLE_GNSS

#include "GNSSModule.h"
#include <Cellular.h>
#include <Config.h>
#include <EventBus.h>
#include <MQTTClient.h>
#include <Log.h>
#include <ArduinoJson.h>

static const char* TAG = "GNSS";

// Load GNSS config. Receiver is not powered until the first read window;
// see loop() for the time-share rationale.
void GNSSModule::begin() {
  JsonObject cfg = Config::get();
  _intervalMs         = (uint32_t)(cfg["gnss"]["interval_s"]      | 30) * 1000UL;
  _coldFixMs          = (uint32_t)(cfg["gnss"]["cold_fix_s"]      | 60) * 1000UL;
  _warmFixMs          = (uint32_t)(cfg["gnss"]["warm_fix_s"]      | 10) * 1000UL;
  _publishWithoutFix  = cfg["gnss"]["publish_without_fix"] | false;

  char msg[96];
  // TODO: migrate to structured logging
  snprintf(msg, sizeof(msg), "Ready - interval %lus, cold fix <=%lus, warm fix <=%lus",
           (unsigned long)(_intervalMs / 1000),
           (unsigned long)(_coldFixMs / 1000),
           (unsigned long)(_warmFixMs / 1000));
  Log::info(TAG, msg);
}

// Acquire one fix per interval via the atomic enable/wait/disable/CFUN=1
// cycle in Cellular. Receiver is OFF between calls so the LTE data path
// stays free for MQTT.
void GNSSModule::loop() {
  uint32_t now = millis();
  if (_lastReadMs != 0 && now - _lastReadMs < _intervalMs) return;
  _lastReadMs = now;

  // GNSS does not depend on cellular registration / MQTT; it just needs
  // the modem powered. If something else (CellularModule activation)
  // already brought the modem up, powerOn() is a no-op. Otherwise it
  // brings up PMU + wakeModem so this GNSS read can proceed.
  if (!Cellular::isModemAlive()) {
    if (!Cellular::powerOn()) {
      Log::warn(TAG, "Modem powerOn failed - GNSS read deferred");
      return;
    }
  }
  readAndPublish();
}

// Acquire one GNSS sample (atomic enable/wait/disable/CFUN=1) and
// publish via MQTT + EventBus.
void GNSSModule::readAndPublish() {
  float lat, lon, alt, speed;
  int   vSat, uSat;
  uint32_t timeout = _hasFix ? _warmFixMs : _coldFixMs;
  bool fix = Cellular::gpsAcquireFix(timeout, &lat, &lon, &alt, &speed,
                                     &vSat, &uSat);

  if (fix) {
    _hasFix     = true;
    _lat        = lat;
    _lon        = lon;
    _alt        = alt;
    _speedKmh   = speed;
    _satsInView = vSat;
    _satsUsed   = uSat;
    _lastFixMs  = millis();
  } else if (!_publishWithoutFix) {
    Log::warn(TAG, "no fix in this window");
    return;
  }

  JsonDocument doc;
  doc["fix"]      = _hasFix;
  doc["lat"]      = _hasFix ? _lat : 0.0f;
  doc["lon"]      = _hasFix ? _lon : 0.0f;
  doc["alt_m"]    = _hasFix ? _alt : 0.0f;
  doc["speed_kmh"] = _hasFix ? _speedKmh : 0.0f;
  doc["sats_view"] = _satsInView;
  doc["sats_used"] = _satsUsed;
  if (_hasFix) {
    doc["age_s"] = (millis() - _lastFixMs) / 1000;
  }

  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";

  // Per-attribute topics for telemetry ingest (one row per sensor/<metric>,
  // numeric value column only). Mirrors BatteryModule's pattern. Only
  // publish when we actually have a fix - emitting 0.0 for lat/lon would
  // land as
  // valid telemetry rows and skew charts.
  char perTopic[80], val[24];
  snprintf(perTopic, sizeof(perTopic), "%s/sensor/gnss/fix", prefix);
  MQTTClient::publish(perTopic, _hasFix ? "1" : "0");
  if (_hasFix) {
    snprintf(perTopic, sizeof(perTopic), "%s/sensor/gnss/lat", prefix);
    snprintf(val, sizeof(val), "%.6f", _lat);
    MQTTClient::publish(perTopic, val);
    snprintf(perTopic, sizeof(perTopic), "%s/sensor/gnss/lon", prefix);
    snprintf(val, sizeof(val), "%.6f", _lon);
    MQTTClient::publish(perTopic, val);
    snprintf(perTopic, sizeof(perTopic), "%s/sensor/gnss/alt_m", prefix);
    snprintf(val, sizeof(val), "%.1f", _alt);
    MQTTClient::publish(perTopic, val);
    snprintf(perTopic, sizeof(perTopic), "%s/sensor/gnss/speed_kmh", prefix);
    snprintf(val, sizeof(val), "%.1f", _speedKmh);
    MQTTClient::publish(perTopic, val);
  }
  snprintf(perTopic, sizeof(perTopic), "%s/sensor/gnss/sats_view", prefix);
  snprintf(val, sizeof(val), "%d", _satsInView);
  MQTTClient::publish(perTopic, val);
  snprintf(perTopic, sizeof(perTopic), "%s/sensor/gnss/sats_used", prefix);
  snprintf(val, sizeof(val), "%d", _satsUsed);
  MQTTClient::publish(perTopic, val);

  // Combined JSON for Lua / EventBus consumers (rules.lua, scripts).
  char topic[64];
  snprintf(topic, sizeof(topic), "%s/sensor/gnss", prefix);
  char payload[192];
  serializeJson(doc, payload, sizeof(payload));
  MQTTClient::publish(topic, payload);

  EventBus::publish("gnss", doc.as<JsonObject>());

  if (_hasFix) {
    char log[96];
    // TODO: migrate to structured logging
    snprintf(log, sizeof(log), "fix lat=%.6f lon=%.6f alt=%.1fm sats=%d/%d",
             _lat, _lon, _alt, _satsUsed, _satsInView);
    Log::info(TAG, log);
  } else {
    Log::warn(TAG, "no fix yet");
  }
}

// Shell: module.status gnss
void GNSSModule::status(ShellOutput out) {
  char line[96];
  snprintf(line, sizeof(line), "  receiver:  %s",
           Cellular::isModemAlive() ? "time-share (off between reads)"
                                    : "waiting for modem");
  out(line);
  if (_hasFix) {
    snprintf(line, sizeof(line), "  fix:       lat=%.6f lon=%.6f", _lat, _lon);
    out(line);
    snprintf(line, sizeof(line), "  alt:       %.1f m", _alt);
    out(line);
    snprintf(line, sizeof(line), "  speed:     %.1f km/h", _speedKmh);
    out(line);
    snprintf(line, sizeof(line), "  sats:      %d used / %d in view",
             _satsUsed, _satsInView);
    out(line);
    snprintf(line, sizeof(line), "  age:       %lus",
             (unsigned long)((millis() - _lastFixMs) / 1000));
    out(line);
  } else {
    out("  fix:       none yet");
  }
}

MODULE_REGISTER(GNSSModule, PRIORITY_SENSOR)

#endif // ENABLE_GNSS
