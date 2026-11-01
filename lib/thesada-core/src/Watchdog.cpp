// thesada-fw - Watchdog.cpp
// SPDX-License-Identifier: GPL-3.0-only

#include "Watchdog.h"
#include "Shell.h"
#include "boot_wait_policy.h"
#include <Arduino.h>
#include <esp_task_wdt.h>

void Watchdog::feed() { esp_task_wdt_reset(); }

void Watchdog::waitStep(uint32_t ms) {
  Shell::pumpConsole();
  delay(waitStepMs(ms));
  feed();
}
