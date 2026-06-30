// thesada-fw - mqtt_rollback_policy.h
// Pure predicate for the mqtt last-good rollback decision. No NVS/Config deps,
// so it is host-unit-testable. See the rollback invariant for the contract.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string.h>

// Roll back to last-good iff a snapshot exists (lg / haveLg), the still-current
// config equals the recorded failing candidate (rbCfg), and it differs from
// last-good. A merely-offline broker (cur == lg) and a recovery edit made after
// an unrelated streak (cur != rbCfg) both return false.
inline bool mqttRollbackShould(const char* lg, bool haveLg,
                               const char* rbCfg, const char* cur) {
  if (!haveLg || !lg || !*lg) return false;   // no snapshot to fall back to
  if (!rbCfg || !*rbCfg) return false;        // no failing candidate recorded
  if (!cur) return false;
  if (strcmp(cur, rbCfg) != 0) return false;  // current != the config that failed
  if (strcmp(cur, lg) == 0) return false;     // current == last-good
  return true;
}
