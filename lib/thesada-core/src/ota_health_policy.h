// thesada-fw - ota_health_policy.h
// When a pending OTA image may be marked valid. Host-unit-testable.
// See docs/invariants.md.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

// A boot that never reaches this, and never connects, is rolled back by the
// bootloader on the next reset. Five minutes covers a broker that is down.
#define OTA_HEALTH_MIN_MS (5u * 60u * 1000u)

// Mark a pending image after MQTT connects or OTA_HEALTH_MIN_MS up.
// in: pending, mqtt up this boot, uptime ms. out: true to confirm.
inline bool otaHealthShouldMark(bool pending, bool mqttUp, uint32_t uptimeMs) {
  if (!pending) return false;
  if (mqttUp) return true;
  return uptimeMs >= OTA_HEALTH_MIN_MS;
}
