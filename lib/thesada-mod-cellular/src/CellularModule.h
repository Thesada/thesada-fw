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
  const char* configKey() override { return "cellular"; }
  bool coreModule() override { return true; }  // fallback transport
  void status(ShellOutput out) override;

private:
  enum class State : uint8_t { STANDBY, ACTIVATING, ACTIVE };

  void subscribeEvents();
  void emitActive(int active);
  void emitRssi();
  void syncClock();

  static State    _state;
  static uint32_t _wifiDownSince;
  static uint32_t _wifiUpSince;
  static uint8_t  _actFails;
  static uint32_t _lastTelemetryMs;
  static uint8_t  _ntpTries;  // cellular NTP tries since activation
  static uint32_t _ntpAt;     // millis() of the next try
  static uint8_t  _ntpIdx;    // next server in the current attempt
};
