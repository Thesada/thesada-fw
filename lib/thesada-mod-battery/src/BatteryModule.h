// thesada-fw - BatteryModule.h
// Periodic battery state publish via MQTT and EventBus.
// Reads voltage, percent, and charging state from PowerManager (AXP2101).
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <Module.h>

class BatteryModule : public Module {
public:
  void begin() override;
  void loop() override;
  const char* name() override { return "BatteryModule"; }
  const char* configKey() override { return "battery"; }
  void status(ShellOutput out) override;
  void selftest(ShellOutput out) override;

private:
  void readAndPublish();

  uint32_t _lastRead   = 0;
  uint32_t _intervalMs = 60000;
};
