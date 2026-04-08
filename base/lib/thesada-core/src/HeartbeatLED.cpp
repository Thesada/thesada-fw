// thesada-fw - HeartbeatLED.cpp
// SPDX-License-Identifier: GPL-3.0-only

#include "HeartbeatLED.h"
#include "Config.h"
#include "Log.h"
#include <thesada_config.h>

#ifdef ENABLE_PMU
#include <PowerManager.h>
#endif

static constexpr uint32_t PULSE_MS = 150;
static const char* TAG = "Heartbeat";

int      HeartbeatLED::_pin        = -1;
bool     HeartbeatLED::_activeLow  = false;
int32_t  HeartbeatLED::_intervalMs = -1;
uint32_t HeartbeatLED::_lastBeat   = 0;
uint32_t HeartbeatLED::_offAt      = 0;
bool     HeartbeatLED::_usePmu     = false;

// Initialize heartbeat LED from config (PMU or GPIO)
void HeartbeatLED::begin() {
  JsonObject cfg     = Config::get();
  int32_t interval_s = cfg["device"]["heartbeat_s"] | -1;

  if (interval_s < 0) {
    Log::info(TAG, "Disabled");
    return;
  }
  if (interval_s < 5) interval_s = 5;
  _intervalMs = interval_s * 1000;

  // Decide which LED driver to use.
#ifdef ENABLE_PMU
  if (PowerManager::isPmuOk()) {
    _usePmu = true;
    Log::info(TAG, "Using AXP2101 LED");
  }
#endif

  if (!_usePmu) {
    int rawPin = cfg["device"]["heartbeat_pin"] | 0;
    if (rawPin == 0) {
      Log::info(TAG, "No PMU and no heartbeat_pin - disabled");
      _intervalMs = -1;
      return;
    }
    _activeLow = (rawPin < 0);
    _pin = _activeLow ? -rawPin : rawPin;
    pinMode(_pin, OUTPUT);
    digitalWrite(_pin, _activeLow ? HIGH : LOW);  // off state
    char msg[48];
    snprintf(msg, sizeof(msg), "Using GPIO %d%s", _pin, _activeLow ? " (active low)" : "");
    Log::info(TAG, msg);
  }

  _lastBeat = millis();
}

// Pulse the LED on the configured interval
void HeartbeatLED::loop() {
  if (_intervalMs < 0) return;

  uint32_t now = millis();

  // Turn off after pulse.
  if (_offAt && now >= _offAt) {
#ifdef ENABLE_PMU
    if (_usePmu) {
      PowerManager::ledOff();
    } else
#endif
    {
      digitalWrite(_pin, _activeLow ? HIGH : LOW);
    }
    _offAt = 0;
  }

  // Pulse on interval.
  if (now - _lastBeat >= (uint32_t)_intervalMs) {
    _lastBeat = now;
    _offAt    = now + PULSE_MS;
#ifdef ENABLE_PMU
    if (_usePmu) {
      PowerManager::ledOn();
    } else
#endif
    {
      digitalWrite(_pin, _activeLow ? LOW : HIGH);
    }
  }
}
