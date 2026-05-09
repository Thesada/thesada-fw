// thesada-fw - GNSSModule.h
// Optional SIM7080G GNSS reader. Mobile asset / tracker scenario.
//
// Reads lat/lon/alt/speed/sats from the SIM7080G's built-in multi-
// constellation GNSS receiver via Cellular::gps* helpers (which share the
// same TinyGsm modem instance Cellular owns). Publishes to
// <prefix>/sensor/gnss as JSON and emits a "gnss" EventBus event for Lua.
//
// Requires ENABLE_GNSS + ENABLE_CELLULAR. Modem must be alive
// (Cellular::isModemAlive()) before the first read - GNSSModule waits
// patiently until that is true so it can be safely included on a board
// that uses cellular only as failover.
//
// Power: SIM7080G GNSS draws ~30 mA on top of the modem rails. Disable
// when not needed. Cold-start TTFF can be 30-60 s; warm-start a few s.
//
// Time-share with LTE: SIM7080G suspends the LTE data path while
// CGNSPWR=1, so this module does NOT keep GNSS powered between reads.
// Each interval acquires a fix via Cellular::gpsAcquireFix which runs
// the full enable -> wait -> disable -> CFUN=1 cycle atomically.
//
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <ModuleRegistry.h>
#include <Shell.h>

class GNSSModule : public Module {
public:
  void begin() override;
  void loop() override;
  const char* name() override { return "GNSS"; }
  void status(ShellOutput out) override;

private:
  bool     _enabled       = false;  // config gnss.enabled
  uint32_t _intervalMs    = 30000;
  uint32_t _lastReadMs    = 0;
  uint32_t _coldFixMs     = 60000;  // upper bound for first fix
  uint32_t _warmFixMs     = 10000;  // upper bound for subsequent fixes
  bool     _publishWithoutFix = false;  // emit even if no fix yet

  // Last fix cached for shell `module.status` and for the "gnss" event payload.
  bool  _hasFix      = false;
  float _lat         = 0.0f;
  float _lon         = 0.0f;
  float _alt         = 0.0f;
  float _speedKmh    = 0.0f;
  int   _satsInView  = 0;
  int   _satsUsed    = 0;
  uint32_t _lastFixMs = 0;

  void readAndPublish();
};
