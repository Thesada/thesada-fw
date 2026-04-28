// thesada-fw - CellularModule.h
// Owns the WiFi-vs-cellular network-selection policy:
//   STANDBY -> ACTIVATING -> ACTIVE -> STANDBY (with hysteresis on both edges)
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Module.h>
#include <stdint.h>

class CellularModule : public Module {
public:
  void begin() override;
  void loop() override;
  const char* name() override { return "CellularModule"; }
  void status(ShellOutput out) override;

private:
  enum class State : uint8_t { STANDBY, ACTIVATING, ACTIVE };

  void subscribeEvents();
  void emitActive(int active);
  void emitRssi();

  static State    _state;
  static uint32_t _wifiDownSince;
  static uint32_t _wifiUpSince;
  static uint32_t _lastTelemetryMs;
};
