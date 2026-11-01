// thesada-fw - temp_guard_policy.h
// DS18B20 plausibility guard. A reading more than max_delta_c from the last
// accepted one is held back; the next reading within max_delta_c of the held
// one is accepted, so a real step costs one interval instead of a reboot.
// Pure, no Arduino deps, host-unit-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cmath>

// DallasTemperature's DEVICE_DISCONNECTED_C; getTempC maps every fault to it.
constexpr float TEMP_NO_READING = -127.0f;

// Per-probe state. accepted: last reading let through. held: last reading
// turned away, which the next reading may confirm. NO_READING = none yet.
struct TempGuard {
  float accepted = TEMP_NO_READING;
  float held     = TEMP_NO_READING;
};

enum class TempVerdict : unsigned char {
  Accepted,     // near the last accepted reading, first reading, or guard off
  Rebaselined,  // far from the last accepted, near the held one: accepted
  NoReading,    // read failed or NaN; state untouched, a held reading survives
  Implausible,  // far from both; it becomes the held reading
};

// Judge one reading and update the guard.
// in: guard state, reading in C, max_delta_c (<= 0 turns the guard off).
// out: verdict. Only Accepted and Rebaselined carry a value to publish.
inline TempVerdict tempGuardJudge(TempGuard& g, float c, float maxDelta) {
  if (std::isnan(c) || c <= TEMP_NO_READING) return TempVerdict::NoReading;

  bool guardOn = maxDelta > 0.0f && g.accepted > TEMP_NO_READING;
  TempVerdict v = TempVerdict::Accepted;
  if (guardOn && std::fabs(c - g.accepted) > maxDelta) {
    if (g.held <= TEMP_NO_READING || std::fabs(c - g.held) > maxDelta) {
      g.held = c;
      return TempVerdict::Implausible;
    }
    v = TempVerdict::Rebaselined;
  }
  g.accepted = c;
  g.held     = TEMP_NO_READING;
  return v;
}
