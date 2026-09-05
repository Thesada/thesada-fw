// thesada-fw - ExampleTemperatureMqtt.h
// Starter module: read one DS18B20 and publish it. The sensor module shape in
// its smallest form - config in begin(), a timer in loop(), one publish path.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <Module.h>

class OneWire;
class DallasTemperature;

class ExampleTemperatureMqtt : public Module {
public:
  void begin() override;
  void loop() override;
  const char* name() override { return "ExampleTemperatureMqtt"; }
  const char* configKey() override { return "example_temperature_mqtt"; }
  void status(ShellOutput out) override;

private:
  void readAndPublish();

  OneWire*           _wire    = nullptr;
  DallasTemperature* _sensors = nullptr;
  uint32_t           _intervalMs = 60000;
  uint32_t           _lastRead   = 0;
  float              _lastC      = 0.0f;   // set to the driver sentinel in begin()
  char               _name[32]   = "example";
};
