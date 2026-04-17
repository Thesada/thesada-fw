// thesada-fw - BatteryModule.h
// Periodic battery state publish via MQTT and EventBus.
// Reads voltage, percent, and charging state from PowerManager (AXP2101).
// Fires a low-battery alert via EventBus when percent drops below battery.low_pct.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <Module.h>

class BatteryModule : public Module {
public:
  void begin() override;
  void loop() override;
  const char* name() override { return "BatteryModule"; }
  void status(ShellOutput out) override;
  void selftest(ShellOutput out) override;

private:
  void readAndPublish();

  uint32_t _lastRead   = 0;
  uint32_t _intervalMs = 60000;
  int      _lowPct     = 20;
  bool     _alertFired = false;  // hysteresis - only fire once per dip
  bool     _disabled   = false;
};
