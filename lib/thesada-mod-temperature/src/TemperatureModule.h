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
  uint8_t       busIdx   = 0;        // which _buses entry this probe lives on
};

// One physical 1-Wire bus (a GPIO with its own OneWire + Dallas driver).
struct TempBus {
  uint8_t            pin;
  OneWire*           wire    = nullptr;
  DallasTemperature* sensors = nullptr;
};

class TemperatureModule : public Module {
public:
  void begin() override;
  void loop() override;
  const char* name() override { return "TemperatureModule"; }
  const char* configKey() override { return "temperature"; }
  void status(ShellOutput out) override;

  // One-shot read for the SensorRegistry callback. Writes one line per
  // discovered probe: `<name>: <temp> C|F`.
  void sensorRead(ShellOutput out);

private:
  void        addBus(uint8_t pin);
  void        discoverSensors();
  void        loadConfigSensors();
  void        saveDiscovered();
  void        readAndPublish();
  void        addressToStr(DeviceAddress addr, char* out);

  // temp.discover [--prune] shell handler: re-search bus live, no reboot.
  void   discoverCmd(int argc, char** argv, ShellOutput out);
  // Drop list+config entries that no longer respond. Returns count removed.
  size_t pruneSensors();
  void   removeSensorsFromConfig(const std::vector<std::string>& addrs);
  // Read one probe: lib retries + reject implausible jump. -127 if unreadable.
  float  readSensorC(TempSensor& s);

  std::vector<TempBus>    _buses;       // one OneWire bus per configured pin
  std::vector<TempSensor> _sensorList;  // probes across all buses, ROM-unique

  uint32_t _lastRead      = 0;
  uint32_t _intervalMs    = 60000;
  uint32_t _conversionMs  = 750;
  bool     _autoDiscover  = true;
  bool     _useFahrenheit = false;
  uint8_t  _readRetries   = 2;      // getTempC retries on transient bus fault
  float    _maxDeltaC     = 40.0f;  // reject jumps > this from last good (0=off)
};
