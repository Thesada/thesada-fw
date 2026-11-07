// thesada-fw - cell_recover_policy.h
// Modem socket-failure recovery: the step a failure streak earns, and the
// task that runs it. Pure, host-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

// Bits, so a step queued from another task merges with fetch_or.
enum : uint8_t { CELL_RECOVER_NONE = 0, CELL_RECOVER_RECYCLE = 1, CELL_RECOVER_RESET = 2 };

// Failures in a row that recycle the bearer, and that power-cycle the modem.
static const uint8_t CELL_RECYCLE_STREAK = 3;
static const uint8_t CELL_RESET_STREAK   = 6;

// in: the streak after this failure. out: the step it earns, or NONE.
inline uint8_t cellRecoverStep(uint8_t streak) {
  if (streak >= CELL_RESET_STREAK) return CELL_RECOVER_RESET;
  return streak == CELL_RECYCLE_STREAK ? CELL_RECOVER_RECYCLE : CELL_RECOVER_NONE;
}

// in: queued step bits. out: the one step to run, a reset over a recycle.
inline uint8_t cellRecoverPick(uint8_t queued) {
  if (queued & CELL_RECOVER_RESET) return CELL_RECOVER_RESET;
  return (queued & CELL_RECOVER_RECYCLE) ? CELL_RECOVER_RECYCLE : CELL_RECOVER_NONE;
}

// The task that drives activation and the health probe runs recovery.
// in: calling task, driver (nullptr before activation). out: run it here.
inline bool cellRecoverHere(const void* task, const void* driver) {
  return driver == nullptr || task == driver;
}
