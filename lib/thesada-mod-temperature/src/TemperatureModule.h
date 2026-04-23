// thesada-fw - TemperatureModule.h
// DS18B20 1-Wire temperature sensing
// Auto-discovers sensors on boot, merges with config.json names
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Module.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <vector>
#include <string>

struct TempSensor {
  DeviceAddress address;
  char          addressStr[17];
  char          name[32];
  float         lastTemp = -127.0f;  // last known good reading
};

class TemperatureModule : public Module {
public:
  void begin() override;
  void loop() override;
  const char* name() override { return "TemperatureModule"; }
  void status(ShellOutput out) override;

  // One-shot read for the SensorRegistry callback. Writes one line per
  // discovered probe: `<name>: <temp> C|F`.
  void sensorRead(ShellOutput out);

private:
  void        discoverSensors();
  void        loadConfigSensors();
  void        mergeNames();
  void        saveDiscovered();
  void        readAndPublish();
  void        addressToStr(DeviceAddress addr, char* out);
  std::string buildTopic();

  OneWire*           _wire    = nullptr;
  DallasTemperature* _sensors = nullptr;

  std::vector<TempSensor> _sensorList;

  uint32_t _lastRead      = 0;
  uint32_t _intervalMs    = 60000;
  uint32_t _conversionMs  = 750;
  bool     _autoDiscover  = true;
  bool     _useFahrenheit = false;
  uint8_t  _pin           = 4;
};
