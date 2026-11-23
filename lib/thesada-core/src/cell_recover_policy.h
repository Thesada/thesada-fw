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

// Failure streaks kept per modem socket slot, so a success on one slot does
// not clear the streak of a slot that keeps failing.
template <uint8_t N>
struct CellFailStreaks {
  uint8_t n[N] = {};

  // in: slot. out: the step this failure earns. A reset clears every slot.
  uint8_t fail(uint8_t slot) {
    if (slot >= N) return CELL_RECOVER_NONE;
    const uint8_t step = cellRecoverStep(++n[slot]);
    if (step == CELL_RECOVER_RESET) {
      for (uint8_t i = 0; i < N; i++) n[i] = 0;
    }
    return step;
  }
  void    ok(uint8_t slot) { if (slot < N) n[slot] = 0; }
  uint8_t streak(uint8_t slot) const { return slot < N ? n[slot] : 0; }
};
