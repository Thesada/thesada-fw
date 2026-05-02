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

  // One-shot read for the SensorRegistry callback. Writes two lines
  // (temperature, humidity) or an error line. Returns nothing.
  void sensorRead(ShellOutput out);

private:
  void readAndPublish();

  // Publish HA MQTT discovery (retained) for temperature + humidity. Idempotent;
  // sets _haPublished on success. Called from loop() once MQTT is connected,
  // because module begin() runs during boot before WiFi/MQTT are up - calling
  // publishRetained then short-circuits and the configs never land (#199).
  // in: none (reads Config). out: publishes 2 retained MQTT messages.
  void publishHaDiscovery();

  bool     _ok           = false;
  bool     _haPublished  = false;
  uint8_t  _addr         = 0x44;
  uint32_t _intervalMs   = 30000;
  uint32_t _lastRead     = 0;
  float    _lastTemp     = 0;
  float    _lastHumid    = 0;
};
