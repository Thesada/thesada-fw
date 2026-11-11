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

  Log::kvf(TAG, "gnss.ready interval_s=%lu cold_fix_s=%lu warm_fix_s=%lu",
           (unsigned long)(_intervalMs / 1000),
           (unsigned long)(_coldFixMs / 1000),
           (unsigned long)(_warmFixMs / 1000));
}

// Acquire one fix per interval via the atomic enable/wait/disable/CFUN=1
// cycle in Cellular. Receiver is OFF between calls so the LTE data path
// stays free for MQTT.
void GNSSModule::loop() {
  uint32_t now = millis();
  if (_readOnce && now - _lastReadMs < _intervalMs) return;
  _readOnce   = true;
  _lastReadMs = now;

  // GNSS does not depend on cellular registration / MQTT; it just needs
  // the modem powered. If something else (CellularModule activation)
  // already brought the modem up, powerOn() is a no-op. Otherwise it
  // brings up PMU + wakeModem so this GNSS read can proceed.
  if (!Cellular::isModemAlive()) {
    if (!Cellular::powerOn()) {
      Log::warn(TAG, "gnss.read_deferred reason=modem_power_on_failed");
      return;
    }
  }
  readAndPublish();
  // From the end of the read: a cold fix that timed out does not start the next at once.
  _lastReadMs = millis();
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
    Log::warn(TAG, "gnss.no_fix action=skip_publish");
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
  char perTopic[MQTT_TOPIC_CAP], val[24];
  auto pub = [&](const char* metric, const char* value) {
    if (MQTTClient::formatTopic(perTopic, sizeof(perTopic), "%s/sensor/gnss/%s", prefix, metric))
      MQTTClient::publish(perTopic, value);
  };
  pub("fix", _hasFix ? "1" : "0");
  if (_hasFix) {
    snprintf(val, sizeof(val), "%.6f", _lat);
    pub("lat", val);
    snprintf(val, sizeof(val), "%.6f", _lon);
    pub("lon", val);
    snprintf(val, sizeof(val), "%.1f", _alt);
    pub("alt_m", val);
    snprintf(val, sizeof(val), "%.1f", _speedKmh);
    pub("speed_kmh", val);
  }
  snprintf(val, sizeof(val), "%d", _satsInView);
  pub("sats_view", val);
  snprintf(val, sizeof(val), "%d", _satsUsed);
  pub("sats_used", val);

  // Combined JSON for Lua / EventBus consumers (rules.lua, scripts).
  char topic[MQTT_TOPIC_CAP];
  if (MQTTClient::formatTopic(topic, sizeof(topic), "%s/sensor/gnss", prefix)) {
    char payload[192];
    serializeJson(doc, payload, sizeof(payload));
    MQTTClient::publish(topic, payload);
  }

  EventBus::publish("gnss", doc.as<JsonObject>());

  if (_hasFix) {
    Log::kvf(TAG, "gnss.fix lat=%.6f lon=%.6f alt_m=%.1f sats_used=%d sats_view=%d",
             _lat, _lon, _alt, _satsUsed, _satsInView);
  } else {
    Log::warn(TAG, "gnss.no_fix");
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
