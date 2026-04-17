// thesada-fw - PowerManager.h
// AXP2101 PMU management: power path config, battery monitoring, heartbeat LED.
//
// Handles all interaction with the AXP2101 on the LILYGO T-SIM7080-S3:
//   - VBUS power path (accept dumb USB chargers and solar input)
//   - TS pin and charging configuration
//   - Battery voltage/percent/charging state via onboard ADC
//   - CHGLED heartbeat pulse (optional, device.heartbeat_s in config)
//
// Uses Wire1 (SDA=15, SCL=7) - does not share state with Wire (ADS1115).
//
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>

class PowerManager {
public:
  // Call once at boot after Config::load(). Must run before ModuleRegistry.
  static void begin();

  // Call every loop() cycle.
  static void loop();

  // Battery accessors - valid after begin(). Check isPmuOk() before using.
  static bool  isPmuOk();
  static bool  isBatteryPresent();
  static float getVoltage();   // V, 0.0 if not detected
  static int   getPercent();   // 0-100, or -1 if not detected
  static bool  isCharging();

  // LED control - called by HeartbeatLED (core) when PMU is available.
  static void ledOn();
  static void ledOff();

private:
  static bool     _pmuOk;
};
