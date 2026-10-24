// thesada-fw - ExampleBlinkLed.h
// Starter module: blink an LED on a configurable timer. The periodic output
// shape - config in begin(), non-blocking timer in loop(), status() output.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <Module.h>

class ExampleBlinkLed : public Module {
public:
  void begin() override;
  void loop() override;
  const char* name() override { return "ExampleBlinkLed"; }
  const char* configKey() override { return "example_blink_led"; }
  void status(ShellOutput out) override;

private:
  uint32_t _intervalMs = 500;
  uint32_t _last       = 0;
  int      _pin        = 2;
  bool     _on         = false;
};
