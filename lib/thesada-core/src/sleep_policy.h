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

// Awake time past the OTA health gate, so its confirm gets a loop turn first.
constexpr uint32_t SLEEP_OTA_GRACE_MS = 30000;

// A wake runs the bootloader, which rolls back an image still pending. So
// sleep waits while the running image awaits its confirm, up to the health
// gate's uptime plus the grace. Past that the image is not confirming.
// in: image awaiting confirm, uptime ms, health gate ms. out: true to stay awake.
inline bool sleepHeldForOta(bool awaitingConfirm, uint32_t uptimeMs, uint32_t healthMs) {
  return awaitingConfirm && uptimeMs < healthMs + SLEEP_OTA_GRACE_MS;
}

// sleepHeldForOta on a fresh confirm try: a long loop turn can pass the gate
// after OTA's own try. Ota: static confirmIfHealthy(), awaitingConfirm() -> bool.
// in: uptime ms, health gate ms, pending out. out: true to stay awake.
template <typename Ota>
inline bool sleepHeldForOtaNow(uint32_t uptimeMs, uint32_t healthMs, bool& pending) {
  Ota::confirmIfHealthy();
  pending = Ota::awaitingConfirm();
  return sleepHeldForOta(pending, uptimeMs, healthMs);
}
