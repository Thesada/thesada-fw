// thesada-fw - TemperatureModule.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include <thesada_config.h>
#include "TemperatureModule.h"
#include <Config.h>
#include <EventBus.h>
#include <MQTTClient.h>
#include <Log.h>
#include <LittleFS.h>
#include <ModuleRegistry.h>

static const char* TAG             = "Temp";
static const char* DISCOVERED_PATH = "/discovered_sensors.json";

// Initialize OneWire bus, load/discover sensors, and log readiness
void TemperatureModule::begin() {
  JsonObject cfg = Config::get();

  _pin            = cfg["temperature"]["pin"]                 | 4;
  _intervalMs     = (uint32_t)(cfg["temperature"]["interval_s"] | 60) * 1000;
  _autoDiscover   = cfg["temperature"]["auto_discover"]       | true;
  _conversionMs   = cfg["temperature"]["conversion_wait_ms"]  | 750;
  if (_conversionMs > 1000) _conversionMs = 1000;
  const char* unit = cfg["temperature"]["unit"] | "C";
  _useFahrenheit  = (unit[0] == 'F' || unit[0] == 'f');

  _wire    = new OneWire(_pin);
  _sensors = new DallasTemperature(_wire);
  _sensors->begin();

  loadConfigSensors();

  if (_autoDiscover) {
    discoverSensors();
    mergeNames();
    saveDiscovered();
  }

  char msg[64];
  snprintf(msg, sizeof(msg), "Ready - %d sensor(s) on GPIO%d", (int)_sensorList.size(), _pin);
  Log::info(TAG, msg);
}

// Trigger a read-and-publish cycle at the configured interval
void TemperatureModule::loop() {
  uint32_t now = millis();
  if (now - _lastRead >= _intervalMs) {
    _lastRead = now;
    readAndPublish();
  }
}

// Load named sensor definitions (address + name) from config.json
void TemperatureModule::loadConfigSensors() {
  JsonObject cfg = Config::get();
  JsonArray  arr = cfg["temperature"]["sensors"].as<JsonArray>();
  if (arr.isNull()) return;

  for (JsonObject s : arr) {
    const char* addrStr = s["address"] | "";
    const char* sname   = s["name"]    | "";
    if (strlen(addrStr) != 16) continue;

    TempSensor sensor;
    strncpy(sensor.addressStr, addrStr, sizeof(sensor.addressStr) - 1);
    sensor.addressStr[16] = '\0';
    strncpy(sensor.name, (strlen(sname) > 0) ? sname : addrStr, sizeof(sensor.name) - 1);
    sensor.name[sizeof(sensor.name) - 1] = '\0';

    for (int i = 0; i < 8; i++) {
      char byte[3] = { addrStr[i * 2], addrStr[i * 2 + 1], '\0' };
      sensor.address[i] = (uint8_t)strtol(byte, nullptr, 16);
    }

    _sensorList.push_back(sensor);
  }

  char msg[64];
  snprintf(msg, sizeof(msg), "Loaded %d sensor(s) from config", (int)_sensorList.size());
  Log::info(TAG, msg);
}

// Scan the OneWire bus and add any new sensors not already in the list
void TemperatureModule::discoverSensors() {
  int found = _sensors->getDeviceCount();

  char msg[64];
  snprintf(msg, sizeof(msg), "Discovered %d sensor(s) on bus", found);
  Log::info(TAG, msg);

  for (int i = 0; i < found; i++) {
    DeviceAddress addr;
    if (!_sensors->getAddress(addr, i)) continue;

    char addrStr[17];
    addressToStr(addr, addrStr);

    bool exists = false;
    for (auto& s : _sensorList) {
      if (strcmp(s.addressStr, addrStr) == 0) { exists = true; break; }
    }

    if (!exists) {
      TempSensor sensor;
      memcpy(sensor.address, addr, sizeof(DeviceAddress));
      strncpy(sensor.addressStr, addrStr, sizeof(sensor.addressStr) - 1);
      sensor.addressStr[16] = '\0';
      strncpy(sensor.name, addrStr, sizeof(sensor.name) - 1);
      sensor.name[sizeof(sensor.name) - 1] = '\0';
      _sensorList.push_back(sensor);

      char log[64];
      snprintf(log, sizeof(log), "New sensor discovered: %s", addrStr);
      Log::info(TAG, log);
    }
  }
}

// Placeholder for merging user-defined names with discovered sensors
void TemperatureModule::mergeNames() {}

// Persist newly discovered sensors to config.json with auto-assigned names
void TemperatureModule::saveDiscovered() {
  if (!LittleFS.begin()) return;

  // ── Load current config.json ───────────────────────────────────────────────
  JsonDocument cfgDoc;
  {
    File f = LittleFS.open("/config.json", "r");
    if (f) { deserializeJson(cfgDoc, f); f.close(); }
  }

  // ── Merge new sensors into temperature.sensors with auto-assigned names ────
  JsonArray cfgSensors;
  if (cfgDoc["temperature"]["sensors"].is<JsonArray>()) {
    cfgSensors = cfgDoc["temperature"]["sensors"].as<JsonArray>();
  } else {
    cfgSensors = cfgDoc["temperature"]["sensors"].to<JsonArray>();
  }

  bool anyNew = false;
  for (auto& s : _sensorList) {
    bool found = false;
    for (JsonObject cs : cfgSensors) {
      if (strcmp(cs["address"] | "", s.addressStr) == 0) { found = true; break; }
    }
    if (!found) {
      char autoName[16];
      snprintf(autoName, sizeof(autoName), "temp_%d", (int)cfgSensors.size() + 1);
      JsonObject ns  = cfgSensors.add<JsonObject>();
      ns["address"]  = s.addressStr;
      ns["name"]     = autoName;
      strncpy(s.name, autoName, sizeof(s.name) - 1);
      s.name[sizeof(s.name) - 1] = '\0';
      anyNew = true;
    }
  }

  if (!anyNew) return; // nothing changed - skip rewrite

  // ── Add default Telegram alert rules if section is empty ──────────────────
  JsonArray alerts;
  if (cfgDoc["telegram"]["alerts"].is<JsonArray>()) {
    alerts = cfgDoc["telegram"]["alerts"].as<JsonArray>();
  }
  if (alerts.isNull() || alerts.size() == 0) {
    JsonArray newAlerts = cfgDoc["telegram"]["alerts"].to<JsonArray>();
    JsonObject hi = newAlerts.add<JsonObject>();
    hi["enabled"]          = false;
    hi["name"]             = "overheat";
    hi["temp_high_c"]      = 40.0;
    hi["message_template"] = "{{sensor.name}}: {{sensor.value}}C -- OVERHEAT (>= {{threshold}}C)";
    JsonObject lo = newAlerts.add<JsonObject>();
    lo["enabled"]          = false;
    lo["name"]             = "freeze";
    lo["temp_low_c"]       = 2.0;
    lo["message_template"] = "{{sensor.name}}: {{sensor.value}}C -- FREEZE RISK (<= {{threshold}}C)";
  }

  // ── Write updated config.json back ─────────────────────────────────────────
  File out = LittleFS.open("/config.json", "w");
  if (!out) { Log::error(TAG, "Failed to write config.json with sensor names"); return; }
  serializeJson(cfgDoc, out);
  out.close();
  Log::info(TAG, "Saved discovered sensors to config.json");
}

// Read all sensors and publish temperatures via MQTT and EventBus
void TemperatureModule::readAndPublish() {
  if (_sensorList.empty()) return;

  _sensors->setWaitForConversion(false);
  _sensors->requestTemperatures();
  delay(_conversionMs);

  JsonDocument doc;
  JsonArray    arr      = doc["sensors"].to<JsonArray>();
  bool         anyValid = false;

  for (auto& s : _sensorList) {
    float temp = _sensors->getTempC(s.address);

    // Retry once on disconnect (common with long wire runs)
    if (temp == DEVICE_DISCONNECTED_C) {
      delay(100);
      temp = _sensors->getTempC(s.address);
    }

    if (temp == DEVICE_DISCONNECTED_C) {
      char log[64];
      snprintf(log, sizeof(log), "Sensor %s disconnected", s.addressStr);
      Log::warn(TAG, log);
      // Still include sensor with last known value so HA discovery works
      if (s.lastTemp != DEVICE_DISCONNECTED_C) {
        float fallbackDisplay = _useFahrenheit ? (s.lastTemp * 9.0f / 5.0f + 32.0f) : s.lastTemp;
        fallbackDisplay = roundf(fallbackDisplay * 100.0f) / 100.0f;
        JsonObject obj = arr.add<JsonObject>();
        obj["name"]    = s.name;
        obj["address"] = s.addressStr;
        obj["temp_c"]  = s.lastTemp;
        obj["temp"]    = fallbackDisplay;
        anyValid = true;
      }
      continue;
    }

    float tempC = roundf(temp * 100.0f) / 100.0f;
    s.lastTemp = tempC;
    float display = _useFahrenheit ? (tempC * 9.0f / 5.0f + 32.0f) : tempC;
    display = roundf(display * 100.0f) / 100.0f;

    JsonObject obj = arr.add<JsonObject>();
    obj["name"]    = s.name;
    obj["address"] = s.addressStr;
    obj["temp_c"]  = tempC;
    obj["temp"]    = display;
    anyValid = true;
  }

  if (!anyValid) return;

  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";

  // Per-sensor topics for HA discovery (simple value in configured unit)
  for (JsonObject s : arr) {
    const char* sname = s["name"] | "unknown";
    float t = s["temp"] | -127.0f;
    if (t == -127.0f) continue;
    char slug[32];
    strncpy(slug, sname, sizeof(slug) - 1);
    slug[sizeof(slug) - 1] = '\0';
    for (char* p = slug; *p; p++) { if (*p == ' ') *p = '_'; *p = tolower(*p); }
    char perTopic[96];
    snprintf(perTopic, sizeof(perTopic), "%s/sensor/temperature/%s", prefix, slug);
    char val[16];
    snprintf(val, sizeof(val), "%.2f", t);
    MQTTClient::publish(perTopic, val);
  }

  // Combined JSON topic (for Lua EventBus, SD logging, backwards compat)
  char topic[64];
  snprintf(topic, sizeof(topic), "%s/sensor/temperature", prefix);
  char payload[512];
  serializeJson(doc, payload, sizeof(payload));
  MQTTClient::publish(topic, payload);

  EventBus::publish("temperature", doc.as<JsonObject>());
}

// Convert an 8-byte OneWire address to a 16-char hex string
void TemperatureModule::addressToStr(DeviceAddress addr, char* out) {
  for (int i = 0; i < 8; i++) snprintf(out + (i * 2), 3, "%02X", addr[i]);
  out[16] = '\0';
}

// Report temperature module status
void TemperatureModule::status(ShellOutput out) {
  JsonObject cfg = Config::get();
  int iv = cfg["temperature"]["interval_s"] | 0;
  int pin = cfg["temperature"]["pin"] | -1;
  JsonArray sensors = cfg["temperature"]["sensors"].as<JsonArray>();
  int cnt = sensors.isNull() ? 0 : sensors.size();
  char line[96];
  snprintf(line, sizeof(line), "%d sensor(s)  pin=%d  interval=%ds", cnt, pin, iv);
  out(line);
}

#ifdef ENABLE_TEMPERATURE
MODULE_REGISTER(TemperatureModule, PRIORITY_SENSOR)
#endif
