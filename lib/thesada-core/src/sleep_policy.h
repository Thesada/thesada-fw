// thesada-fw - sleep_policy.h
// SleepManager's awake window: wake_s bounds and when the window has passed.
// Pure, host-unit-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include "elapsed_policy.h"

constexpr uint32_t SLEEP_WAKE_S_MIN = 10;
constexpr uint32_t SLEEP_WAKE_S_MAX = 86400;

// in: configured wake_s. out: awake span in ms, wake_s held to [MIN, MAX].
inline uint32_t sleepWakeSpanMs(uint32_t wakeS) {
  if (wakeS < SLEEP_WAKE_S_MIN) wakeS = SLEEP_WAKE_S_MIN;
  if (wakeS > SLEEP_WAKE_S_MAX) wakeS = SLEEP_WAKE_S_MAX;
  return wakeS * 1000UL;
}

// in: millis() now, millis() at begin, span. out: true once the span has passed.
inline bool sleepWakeDone(uint32_t now, uint32_t start, uint32_t spanMs) {
  return !msWithin(now, start, spanMs);
}
