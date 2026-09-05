// thesada-fw - ExampleTemperatureMqtt.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include <thesada_config.h>
#include "ExampleTemperatureMqtt.h"
#include <Config.h>
#include <EventBus.h>
#include <MQTTClient.h>
#include <Log.h>
#include <ModuleRegistry.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#ifdef ENABLE_EXAMPLE_TEMPERATURE_MQTT

static const char* TAG = "ExTemp";

// Everything with a side effect lives here, never in the constructor: the
// registry constructs every module at static-init time, before Config exists.
// in: config block example_temperature_mqtt (pin, interval_s, name).
// out: bus + driver allocated, first read on the next loop() after interval.
void ExampleTemperatureMqtt::begin() {
  JsonObject cfg = Config::get();
  int pin     = cfg["example_temperature_mqtt"]["pin"]        | 12;
  _intervalMs = (uint32_t)(cfg["example_temperature_mqtt"]["interval_s"] | 60) * 1000;
  strlcpy(_name, cfg["example_temperature_mqtt"]["name"] | "example", sizeof(_name));
  _lastC = DEVICE_DISCONNECTED_C;

  _wire    = new OneWire(pin);
  _sensors = new DallasTemperature(_wire);
  _sensors->begin();

  Log::kvf(TAG, "example_temp.ready pin=%d interval_s=%lu name=%s",
           pin, (unsigned long)(_intervalMs / 1000), _name);
}

// loop() runs every main-loop pass, so it must not block. The timer is the
// whole scheduler: do the work when the interval has elapsed, otherwise return.
// in: none. out: one readAndPublish() per interval.
void ExampleTemperatureMqtt::loop() {
  uint32_t now = millis();
  if (now - _lastRead < _intervalMs) return;
  _lastRead = now;
  readAndPublish();
}

// Read the first probe on the bus and publish it on MQTT and the event bus.
// in: none. out: <prefix>/sensor/temperature/<name> + EventBus "temperature".
void ExampleTemperatureMqtt::readAndPublish() {
  _sensors->requestTemperatures();
  float c = _sensors->getTempCByIndex(0);
  if (c == DEVICE_DISCONNECTED_C) {
    Log::kvfw(TAG, "example_temp.sensor_disconnected");
    return;
  }
  _lastC = roundf(c * 100.0f) / 100.0f;

  // Two outputs, same as the full temperature module: a plain value on a
  // per-sensor topic for Home Assistant, and a JSON event on the bus so Lua
  // rules and the SD logger see it without knowing about MQTT.
  JsonObject  cfg    = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  char topic[96];
  snprintf(topic, sizeof(topic), "%s/sensor/temperature/%s", prefix, _name);
  char val[16];
  snprintf(val, sizeof(val), "%.2f", _lastC);
  MQTTClient::publish(topic, val);

  JsonDocument doc;
  JsonObject s = doc["sensors"].add<JsonObject>();
  s["name"]   = _name;
  s["temp_c"] = _lastC;
  s["temp"]   = _lastC;
  EventBus::publish("temperature", doc.as<JsonObject>());
}

// One line for `module.status`.
// in: out sink. out: "name=<n> last_c=<c>".
void ExampleTemperatureMqtt::status(ShellOutput out) {
  char line[64];
  snprintf(line, sizeof(line), "name=%s last_c=%.2f", _name, _lastC);
  out(line);
}

MODULE_REGISTER(ExampleTemperatureMqtt, PRIORITY_SENSOR)

#endif  // ENABLE_EXAMPLE_TEMPERATURE_MQTT
