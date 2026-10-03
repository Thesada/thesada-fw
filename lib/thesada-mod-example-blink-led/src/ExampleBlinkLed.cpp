// thesada-fw - ExampleBlinkLed.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include <thesada_config.h>
#include "ExampleBlinkLed.h"
#include <Config.h>
#include <Log.h>
#include <ModuleRegistry.h>
#include <ArduinoJson.h>
#include <stdio.h>

#ifdef ENABLE_EXAMPLE_BLINK_LED

static const char* TAG = "ExBlink";

// Read pin and interval from config, initialize the GPIO.
// in: config block example_blink_led (pin, interval_ms).
// out: GPIO configured as OUTPUT.
void ExampleBlinkLed::begin() {
  JsonObject cfg = Config::get();
  _intervalMs = cfg["example_blink_led"]["interval_ms"] | 500;
  _pin        = cfg["example_blink_led"]["pin"]         | 2;

  pinMode(_pin, OUTPUT);
  digitalWrite(_pin, _on ? HIGH : LOW);

  Log::kvf(TAG, "example_blink.ready pin=%d interval_ms=%u", _pin, (unsigned)_intervalMs);
}

// Non-blocking toggle driven by millis().
// in: millis(). out: pin level toggled every interval_ms.
void ExampleBlinkLed::loop() {
  if (millis() - _last < _intervalMs) {
    return;
  }
  _last = millis();
  _on = !_on;
  digitalWrite(_pin, _on ? HIGH : LOW);
}

// One line for `module.status`.
// in: out sink. out: "pin=<n> state=on|off interval_ms=<n>".
void ExampleBlinkLed::status(ShellOutput out) {
  char line[64];
  snprintf(line, sizeof(line), "pin=%d state=%s interval_ms=%u", _pin, _on ? "on" : "off", (unsigned)_intervalMs);
  out(line);
}

MODULE_REGISTER(ExampleBlinkLed, PRIORITY_OUTPUT)

#endif  // ENABLE_EXAMPLE_BLINK_LED
