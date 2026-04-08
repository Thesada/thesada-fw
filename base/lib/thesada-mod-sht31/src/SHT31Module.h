// thesada-fw - SHT31Module.h
// SHT31 I2C temperature and humidity sensor.
// Publishes to MQTT and EventBus at configurable interval.
// Config: sht31.sda, sht31.scl, sht31.address, sht31.interval_s
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <Module.h>

class SHT31Module : public Module {
public:
  void begin() override;
  void loop() override;
  const char* name() override { return "SHT31"; }
  void status(ShellOutput out) override;
  void selftest(ShellOutput out) override;

private:
  void readAndPublish();

  bool     _ok         = false;
  uint8_t  _addr       = 0x44;
  uint32_t _intervalMs = 30000;
  uint32_t _lastRead   = 0;
  float    _lastTemp   = 0;
  float    _lastHumid  = 0;
};
