// thesada-fw - SHT31Module.cpp
// SHT31 I2C temperature and humidity sensor (address 0x44 or 0x45).
// Raw I2C read - no external library needed.
// SPDX-License-Identifier: GPL-3.0-only

#include <thesada_config.h>
#ifdef ENABLE_SHT31

#include "SHT31Module.h"
#include <Config.h>
#include <EventBus.h>
#include <MQTTClient.h>
#include <Log.h>
#include <Shell.h>
#include <ModuleRegistry.h>
#include <SensorRegistry.h>
#include <Wire.h>
#include <ArduinoJson.h>

static const char* TAG = "SHT31";

// Read temperature and humidity from SHT31 via I2C
static bool sht31Read(uint8_t addr, float& temp, float& humid) {
  Wire.beginTransmission(addr);
  Wire.write(0x24);  // high repeatability, no clock stretching
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;

  delay(20);  // SHT31 measurement time (~15ms for high repeatability)

  Wire.requestFrom(addr, (uint8_t)6);
  if (Wire.available() != 6) return false;

  uint8_t buf[6];
  for (int i = 0; i < 6; i++) buf[i] = Wire.read();

  // Skip CRC checks (bytes 2 and 5) for simplicity
  uint16_t rawTemp  = (buf[0] << 8) | buf[1];
  uint16_t rawHumid = (buf[3] << 8) | buf[4];

  temp  = -45.0f + 175.0f * ((float)rawTemp / 65535.0f);
  humid = 100.0f * ((float)rawHumid / 65535.0f);
  return true;
}

// Initialize I2C and verify sensor is present
void SHT31Module::begin() {
  JsonObject cfg = Config::get();
  int sda  = cfg["sht31"]["sda"]        | 11;
  int scl  = cfg["sht31"]["scl"]        | 12;
  _addr    = cfg["sht31"]["address"]    | 0x44;
  uint32_t iv = cfg["sht31"]["interval_s"] | 30;
  _intervalMs = iv * 1000UL;

  Wire.begin(sda, scl);

  // Probe the sensor. Missing device is expected on bare nodes that share
  // the BOARD_S3_BARE env with sht31 devices - log at info, not error.
  Wire.beginTransmission(_addr);
  uint8_t err = Wire.endTransmission();
  if (err != 0) {
    char msg[64];
    // TODO: migrate to structured logging
    snprintf(msg, sizeof(msg), "no device at 0x%02X (SDA=%d SCL=%d) - sensor absent", _addr, sda, scl);
    Log::info(TAG, msg);
    return;
  }

  _ok = true;
  // HA discovery is deferred to publishHaDiscovery(), invoked from loop()
  // once MQTT is connected. Calling MQTTClient::publishRetained() here would
  // short-circuit (MQTT is not up during module begin) so the configs would
  // never land on the broker.

  // Register under the unified `sensors` CLI. The old standalone
  // `sht31` command is gone - use `sensors sht31` instead.
  SensorRegistry::add("sht31", "temperature + humidity (I2C)",
    [](ShellOutput out, void* ctx) {
      static_cast<SHT31Module*>(ctx)->sensorRead(out);
    }, this, true);

  char msg[64];
  // TODO: migrate to structured logging
  snprintf(msg, sizeof(msg), "Ready - SDA=%d SCL=%d addr=0x%02X interval=%lus", sda, scl, _addr, iv);
  Log::info(TAG, msg);

  // First reading immediately
  readAndPublish();
}

// Periodic sensor read at configured interval. Also drives the deferred
// HA discovery publish: module begin() runs before MQTT connects so
// the discovery configs cannot be published there. Once MQTT is up, the
// next loop() tick lands them and remembers via _haPublished. Discovery
// re-publishes on the next reconnect because MQTTClient::connect() resets
// the retained-topics manifest and re-emits LWT + /info via its own path;
// SHT31 must do the same so the broker keeps the configs after a session
// drop. We watch MQTTClient::connected() transitions: when we observe a
// fresh connect (was disconnected, now connected) we clear _haPublished.
void SHT31Module::loop() {
  if (!_ok) return;

  static bool wasConnected = false;
  bool nowConnected = MQTTClient::connected();
  if (nowConnected && !wasConnected) {
    _haPublished = false;
  }
  wasConnected = nowConnected;

  if (nowConnected && !_haPublished) {
    publishHaDiscovery();
  }

  uint32_t now = millis();
  if (now - _lastRead >= _intervalMs) {
    _lastRead = now;
    readAndPublish();
  }
}

// Build + publish HA MQTT discovery (retained) for SHT31 temperature and
// humidity. Idempotent. Caller must guarantee MQTTClient::connected() is
// true; we set _haPublished on success so loop() won't re-emit until the
// next reconnect transition clears it.
// in: none (reads Config). out: 2 retained MQTT messages on the broker.
void SHT31Module::publishHaDiscovery() {
  JsonObject cfg = Config::get();
  bool haDisc = cfg["mqtt"]["ha_discovery"] | true;
  if (!haDisc) {
    _haPublished = true;  // disabled by config; do not retry
    return;
  }
  const char* prefix  = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  const char* devName = cfg["device"]["friendly_name"] | cfg["device"]["name"] | "Thesada Node";
  const char* devId   = cfg["device"]["name"] | "thesada_node";
  const char* unit    = cfg["temperature"]["unit"] | "C";
  const char* haUnit  = (unit[0] == 'F' || unit[0] == 'f') ? "\xC2\xB0""F" : "\xC2\xB0""C";

  char availTopic[64];
  snprintf(availTopic, sizeof(availTopic), "%s/status", prefix);

  auto disc = [&](const char* uid, const char* name, const char* stateTopic,
                  const char* dUnit, const char* devClass, const char* stateClass) {
    JsonDocument doc;
    doc["name"]        = name;
    doc["stat_t"]      = stateTopic;
    doc["uniq_id"]     = uid;
    doc["avty_t"]      = availTopic;
    if (dUnit && strlen(dUnit) > 0)           doc["unit_of_meas"] = dUnit;
    if (devClass && strlen(devClass) > 0)     doc["dev_cla"]      = devClass;
    if (stateClass && strlen(stateClass) > 0) doc["stat_cla"]     = stateClass;
    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"]  = devId;
    dev["name"] = devName;
    dev["mf"]   = "Thesada";
    dev["sw"]   = FIRMWARE_VERSION;
    char topic[128], payload[512];
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", devId, uid);
    serializeJson(doc, payload, sizeof(payload));
    MQTTClient::publishRetained(topic, payload);
  };

  char uid[48], st[96];
  snprintf(uid, sizeof(uid), "%s_sht31_temp", devId);
  snprintf(st, sizeof(st), "%s/sensor/temperature/sht31", prefix);
  disc(uid, "SHT31 Temperature", st, haUnit, "temperature", "measurement");

  snprintf(uid, sizeof(uid), "%s_sht31_humidity", devId);
  snprintf(st, sizeof(st), "%s/sensor/humidity/sht31", prefix);
  disc(uid, "SHT31 Humidity", st, "%", "humidity", "measurement");

  _haPublished = true;
  Log::info(TAG, "HA discovery published");
}

// Read sensor and publish to MQTT + EventBus
void SHT31Module::readAndPublish() {
  float temp, humid;
  if (!sht31Read(_addr, temp, humid)) {
    Log::warn(TAG, "Read failed");
    return;
  }

  _lastTemp  = temp;
  _lastHumid = humid;

  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  const char* unit = cfg["temperature"]["unit"] | "C";

  float displayTemp = temp;
  if (unit[0] == 'F' || unit[0] == 'f') {
    displayTemp = temp * 9.0f / 5.0f + 32.0f;
  }

  // Per-sensor MQTT topics (HA discovery compatible)
  char topic[96], val[16];
  snprintf(topic, sizeof(topic), "%s/sensor/temperature/sht31", prefix);
  snprintf(val, sizeof(val), "%.1f", displayTemp);
  MQTTClient::publish(topic, val);

  snprintf(topic, sizeof(topic), "%s/sensor/humidity/sht31", prefix);
  snprintf(val, sizeof(val), "%.1f", humid);
  MQTTClient::publish(topic, val);

  // EventBus for Lua alert rules
  JsonDocument doc;
  JsonArray sensors = doc["sensors"].to<JsonArray>();
  JsonObject s = sensors.add<JsonObject>();
  s["name"]   = "SHT31";
  s["temp_c"] = roundf(temp * 10.0f) / 10.0f;
  s["temp"]   = roundf(displayTemp * 10.0f) / 10.0f;
  s["humidity"] = roundf(humid * 10.0f) / 10.0f;
  EventBus::publish("temperature", doc.as<JsonObject>());

  char msg[64];
  // TODO: migrate to structured logging
  snprintf(msg, sizeof(msg), "%.1f%s  %.1f%%", displayTemp, unit, humid);
  Log::info(TAG, msg);
}

// SensorRegistry read callback. Outputs two lines on success, one on error.
void SHT31Module::sensorRead(ShellOutput out) {
  if (!_ok) { out("  not initialized"); return; }
  float t, h;
  if (sht31Read(_addr, t, h)) {
    char line[64];
    snprintf(line, sizeof(line), "  temperature: %.1f C", t);
    out(line);
    snprintf(line, sizeof(line), "  humidity:    %.1f %%", h);
    out(line);
  } else {
    out("  read failed");
  }
}

// Report module status
void SHT31Module::status(ShellOutput out) {
  char line[64];
  if (_ok) {
    snprintf(line, sizeof(line), "addr=0x%02X  %.1fC  %.1f%%", _addr, _lastTemp, _lastHumid);
  } else {
    snprintf(line, sizeof(line), "not initialized");
  }
  out(line);
}

// Run self-test
void SHT31Module::selftest(ShellOutput out) {
  if (!_ok) { out("[WARN] SHT31 not initialized"); return; }
  float t, h;
  if (sht31Read(_addr, t, h)) {
    char line[64];
    snprintf(line, sizeof(line), "[PASS] SHT31: %.1fC %.1f%%", t, h);
    out(line);
  } else {
    out("[FAIL] SHT31 read failed");
  }
}

MODULE_REGISTER(SHT31Module, PRIORITY_SENSOR)

#endif // ENABLE_SHT31
