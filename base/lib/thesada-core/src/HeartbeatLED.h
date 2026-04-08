// thesada-fw - HeartbeatLED.h
// Periodic LED blink. Uses GPIO pin if configured, or AXP2101 LED via
// PowerManager when ENABLE_PMU is active. Always compiled (core).
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>

class HeartbeatLED {
public:
  static void begin();
  static void loop();
private:
  static int      _pin;
  static bool     _activeLow;
  static int32_t  _intervalMs;
  static uint32_t _lastBeat;
  static uint32_t _offAt;
  static bool     _usePmu;
};
