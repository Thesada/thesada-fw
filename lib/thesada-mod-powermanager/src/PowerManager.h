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
#include <thesada_config.h>

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

#ifdef ENABLE_CELLULAR
  // Configure DC3 (modem Vcc) + BLDO2 (antenna rail) for the SIM7080G.
  // Called by Cellular::initPMU at activation time. Routed through the
  // static _pmu instance so the modem rails do not require a second
  // XPowersPMU object - a stack-local one would deinit Wire1 on
  // destruction and break later battery polls.
  // Includes a 200 ms gap between disable/enable so Vcc actually drops
  // below the SIM7080 brownout threshold and the modem starts from a
  // clean state every cold boot.
  static bool setModemRails();

  // Hardware-reset the modem by dropping DC3 for 200 ms, then re-enabling.
  // Use in place of `+CFUN=1,1` for recovery from a wedged modem state.
  // Caller is responsible for re-pulsing PWRKEY and re-establishing the
  // modem session afterwards.
  static bool resetModem();
#endif // ENABLE_CELLULAR

private:
  static bool     _pmuOk;
};
