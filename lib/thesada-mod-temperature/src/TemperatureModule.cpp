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
#include <SensorRegistry.h>
#include <Shell.h>
#include <math.h>

static const char* TAG = "Temp";

// Initialize OneWire bus, load/discover sensors, and log readiness
void TemperatureModule::begin() {
  JsonObject cfg = Config::get();

  _intervalMs     = (uint32_t)(cfg["temperature"]["interval_s"] | 60) * 1000;
  _autoDiscover   = cfg["temperature"]["auto_discover"]       | true;
  _conversionMs   = cfg["temperature"]["conversion_wait_ms"]  | 750;
  if (_conversionMs > 1000) _conversionMs = 1000;
  _readRetries    = cfg["temperature"]["read_retries"]        | 2;
  _maxDeltaC      = cfg["temperature"]["max_delta_c"]         | 40.0f;
  const char* unit = cfg["temperature"]["unit"] | "C";
  _useFahrenheit  = (unit[0] == 'F' || unit[0] == 'f');

  // Bus list: temperature.buses[{pin}] if present, else scalar temperature.pin.
  JsonArray busArr = cfg["temperature"]["buses"].as<JsonArray>();
  if (!busArr.isNull() && busArr.size() > 0) {
    for (JsonObject b : busArr) addBus(b["pin"] | 4);
  } else {
    addBus(cfg["temperature"]["pin"] | 4);
  }

  loadConfigSensors();

  // Always retag configured probes to their real bus; only auto-discover adds
  // and persists previously unknown probes.
  discoverSensors(_autoDiscover);
  if (_autoDiscover) saveDiscovered();

  Log::kvf(TAG, "temp.ready sensors=%d buses=%d",
           (int)_sensorList.size(), (int)_buses.size());

  SensorRegistry::add("temperature", "DS18B20 1-wire probes",
    [](ShellOutput out, void* ctx) {
      static_cast<TemperatureModule*>(ctx)->sensorRead(out);
    }, this, true);

  Shell::registerCommand("temp.discover",
    "Re-scan 1-Wire bus for probes, no reboot (temp.discover [--prune])",
    [this](int argc, char** argv, ShellOutput out) {
      discoverCmd(argc, argv, out);
    });
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

  Log::kvf(TAG, "temp.config_loaded sensors=%d", (int)_sensorList.size());
}

// Create a OneWire + Dallas driver for one GPIO and walk it once.
void TemperatureModule::addBus(uint8_t pin) {
  TempBus bus;
  bus.pin     = pin;
  bus.wire    = new OneWire(pin);
  bus.sensors = new DallasTemperature(bus.wire);
  bus.sensors->begin();
  _buses.push_back(bus);
}

// Scan every bus and re-tag each known probe's busIdx (handles a probe that
// moved buses). New probes are appended only when addNew is set. ROM addresses
// are globally unique so they merge cleanly.
void TemperatureModule::discoverSensors(bool addNew) {
  for (size_t b = 0; b < _buses.size(); b++) {
    DallasTemperature* dt = _buses[b].sensors;
    int found = dt->getDeviceCount();

    Log::kvf(TAG, "temp.bus_scan bus=%u gpio=%u found=%d",
             (unsigned)b, (unsigned)_buses[b].pin, found);

    for (int i = 0; i < found; i++) {
      DeviceAddress addr;
      if (!dt->getAddress(addr, i)) continue;

      char addrStr[17];
      addressToStr(addr, addrStr);

      bool exists = false;
      for (auto& s : _sensorList) {
        if (strcmp(s.addressStr, addrStr) == 0) {
          s.busIdx = (uint8_t)b;  // re-tag in case the probe moved buses
          exists = true;
          break;
        }
      }
      if (exists) continue;
      if (!addNew) continue;

      TempSensor sensor;
      memcpy(sensor.address, addr, sizeof(DeviceAddress));
      strncpy(sensor.addressStr, addrStr, sizeof(sensor.addressStr) - 1);
      sensor.addressStr[16] = '\0';
      strncpy(sensor.name, addrStr, sizeof(sensor.name) - 1);
      sensor.name[sizeof(sensor.name) - 1] = '\0';
      sensor.busIdx = (uint8_t)b;
      _sensorList.push_back(sensor);

      Log::kvf(TAG, "temp.sensor_new bus=%u addr=%s", (unsigned)b, addrStr);
    }
  }
}

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
  if (!out) { Log::error(TAG, "temp.config_write_failed op=save file=config.json"); return; }
  size_t written = serializeJson(cfgDoc, out);
  out.close();
  if (written < measureJson(cfgDoc)) {
    Log::error(TAG, "temp.config_write_failed op=save reason=short_write");
    return;
  }
  Log::info(TAG, "temp.config_saved file=config.json");
}

// temp.discover [--prune]: re-search the live bus (begin() repopulates the
// cached device count), add new probes, optionally drop dead ones, persist.
void TemperatureModule::discoverCmd(int argc, char** argv, ShellOutput out) {
  if (_buses.empty()) { out("temperature module not ready"); return; }

  bool prune = false;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--prune") == 0) prune = true;
  }

  size_t before = _sensorList.size();
  for (auto& b : _buses) b.sensors->begin();  // re-walk every bus
  discoverSensors(true);
  size_t added   = _sensorList.size() - before;
  size_t removed = prune ? pruneSensors() : 0;
  saveDiscovered();

  char line[80];
  for (auto& s : _sensorList) {
    snprintf(line, sizeof(line), "  %-16s %s", s.name, s.addressStr);
    out(line);
  }
  snprintf(line, sizeof(line), "%d sensor(s) (+%d new, -%d pruned)",
           (int)_sensorList.size(), (int)added, (int)removed);
  out(line);

  // Refresh /api/state + MQTT. readAndPublish() early-returns on an empty
  // roster, so publish an empty array directly to clear a pruned sensor.
  if (_sensorList.empty()) {
    JsonDocument doc;
    doc["sensors"].to<JsonArray>();
    EventBus::publish("temperature", doc.as<JsonObject>());
  } else {
    readAndPublish();
  }
}

// Drop sensors that no longer answer on the bus, from list and config.json.
size_t TemperatureModule::pruneSensors() {
  std::vector<std::string> dropped;
  for (auto it = _sensorList.begin(); it != _sensorList.end(); ) {
    bool live = it->busIdx < _buses.size() &&
                _buses[it->busIdx].sensors->isConnected(it->address);
    if (!live) {
      dropped.push_back(it->addressStr);
      it = _sensorList.erase(it);
    } else {
      ++it;
    }
  }
  if (!dropped.empty()) removeSensorsFromConfig(dropped);
  return dropped.size();
}

// Remove the given addresses from temperature.sensors in config.json.
void TemperatureModule::removeSensorsFromConfig(const std::vector<std::string>& addrs) {
  if (!LittleFS.begin()) return;

  JsonDocument cfgDoc;
  {
    File f = LittleFS.open("/config.json", "r");
    if (f) { deserializeJson(cfgDoc, f); f.close(); }
  }
  if (!cfgDoc["temperature"]["sensors"].is<JsonArray>()) return;

  JsonArray src = cfgDoc["temperature"]["sensors"].as<JsonArray>();
  JsonDocument outDoc;
  JsonArray kept = outDoc.to<JsonArray>();
  for (JsonObject s : src) {
    const char* a = s["address"] | "";
    bool drop = false;
    for (auto& d : addrs) { if (d == a) { drop = true; break; } }
    if (!drop) kept.add(s);
  }
  cfgDoc["temperature"]["sensors"].set(kept);

  File w = LittleFS.open("/config.json", "w");
  if (!w) { Log::error(TAG, "temp.config_write_failed op=prune file=config.json"); return; }
  size_t written = serializeJson(cfgDoc, w);
  w.close();
  if (written < measureJson(cfgDoc)) {
    Log::error(TAG, "temp.config_write_failed op=prune reason=short_write");
  }
}

// Read one probe with library retries (CRC-validated) plus a plausibility
// guard. Returns DEVICE_DISCONNECTED_C on read fail or implausible jump.
float TemperatureModule::readSensorC(TempSensor& s) {
  if (s.busIdx >= _buses.size()) return DEVICE_DISCONNECTED_C;
  float temp = _buses[s.busIdx].sensors->getTempC(s.address, _readRetries);
  if (temp == DEVICE_DISCONNECTED_C) return DEVICE_DISCONNECTED_C;

  if (_maxDeltaC > 0.0f && s.lastTemp != DEVICE_DISCONNECTED_C &&
      fabsf(temp - s.lastTemp) > _maxDeltaC) {
    Log::kvfw(TAG, "temp.read_rejected addr=%s last=%.2f got=%.2f reason=implausible",
              s.addressStr, s.lastTemp, temp);
    return DEVICE_DISCONNECTED_C;
  }
  return temp;
}

// Read all sensors and publish temperatures via MQTT and EventBus
void TemperatureModule::readAndPublish() {
  if (_sensorList.empty()) return;

  // Kick a conversion on every bus, then wait once - buses convert in parallel.
  for (auto& b : _buses) {
    b.sensors->setWaitForConversion(false);
    b.sensors->requestTemperatures();
  }
  delay(_conversionMs);

  JsonDocument doc;
  JsonArray    arr      = doc["sensors"].to<JsonArray>();
  bool         anyValid = false;

  for (auto& s : _sensorList) {
    float temp = readSensorC(s);

    if (temp == DEVICE_DISCONNECTED_C) {
      Log::kvfw(TAG, "temp.sensor_disconnected addr=%s", s.addressStr);
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

  // Combined JSON topic (for Lua EventBus, SD logging, backwards compat).
  // Dynamic buffer - multi-bus probe counts can outgrow any fixed size and a
  // truncated payload would publish malformed JSON.
  char topic[64];
  snprintf(topic, sizeof(topic), "%s/sensor/temperature", prefix);
  String payload;
  payload.reserve(measureJson(doc) + 1);
  serializeJson(doc, payload);
  MQTTClient::publish(topic, payload.c_str());

  EventBus::publish("temperature", doc.as<JsonObject>());
}

// Convert an 8-byte OneWire address to a 16-char hex string
void TemperatureModule::addressToStr(DeviceAddress addr, char* out) {
  for (int i = 0; i < 8; i++) snprintf(out + (i * 2), 3, "%02X", addr[i]);
  out[16] = '\0';
}

// SensorRegistry read callback. Triggers a conversion, then prints one line
// per named probe with the latest reading.
void TemperatureModule::sensorRead(ShellOutput out) {
  if (_buses.empty() || _sensorList.empty()) { out("  no probes on bus"); return; }
  for (auto& b : _buses) b.sensors->requestTemperatures();
  delay(_conversionMs);
  char line[96];
  for (auto& s : _sensorList) {
    float c = readSensorC(s);
    if (c == DEVICE_DISCONNECTED_C) {
      snprintf(line, sizeof(line), "  %-16s: disconnected", s.name);
    } else {
      float v = _useFahrenheit ? (c * 9.0f / 5.0f + 32.0f) : c;
      snprintf(line, sizeof(line), "  %-16s: %.2f %s",
               s.name, v, _useFahrenheit ? "F" : "C");
      s.lastTemp = c;
    }
    out(line);
  }
}

// Report temperature module status from the live sensor list, not config -
// config is stale after a live temp.discover (saveDiscovered writes the file,
// not in-memory Config).
void TemperatureModule::status(ShellOutput out) {
  char pins[40] = {0};
  for (size_t b = 0; b < _buses.size(); b++) {
    char p[8];
    snprintf(p, sizeof(p), "%s%u", b ? "," : "", (unsigned)_buses[b].pin);
    strncat(pins, p, sizeof(pins) - strlen(pins) - 1);
  }
  char line[96];
  snprintf(line, sizeof(line), "%d sensor(s)  buses=[%s]  interval=%ds",
           (int)_sensorList.size(), pins, (int)(_intervalMs / 1000));
  out(line);
}

#ifdef ENABLE_TEMPERATURE
MODULE_REGISTER(TemperatureModule, PRIORITY_SENSOR)
#endif
