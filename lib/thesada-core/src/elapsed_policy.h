// thesada-fw - elapsed_policy.h
// millis() deadlines and spans that hold across the 49.7-day wrap. A bare
// `millis() < end` aborts at the wrap. Pure, host-unit-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

// in: millis() now, deadline. out: true once the deadline is reached.
inline bool msReached(uint32_t now, uint32_t at) {
  return (int32_t)(now - at) >= 0;
}

// in: millis() now, start, span. out: true while fewer than span ms have passed.
inline bool msWithin(uint32_t now, uint32_t start, uint32_t span) {
  return now - start < span;
}
