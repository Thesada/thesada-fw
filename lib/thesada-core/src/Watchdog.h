// thesada-fw - Watchdog.h
// Task watchdog feeds for bounded waits. Feed only inside a loop whose exit
// is bounded; an unbounded wait must still trip the watchdog.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

class Watchdog {
public:
  // Reset the task watchdog for the calling task. in: none. out: none.
  static void feed();

  // One step of a bounded wait: pump the console, sleep, feed.
  // in: sleep ms, clamped by waitStepMs(). out: none.
  static void waitStep(uint32_t ms);
};
