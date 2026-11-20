// thesada-fw - module_begin_policy.h
// Module begin() order with task watchdog feeds between modules, never inside one.
// Pure, host-unit-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

// Feeds once before the first module and once after each, so a slow neighbour
// never counts against the next module, and a module that hangs still trips.
// in: module count, begin(i) runs entry i, feed() resets the task watchdog.
template <class BeginFn, class FeedFn>
void beginModulesFed(uint8_t count, BeginFn begin, FeedFn feed) {
  feed();
  for (uint8_t i = 0; i < count; i++) {
    begin(i);
    feed();
  }
}
