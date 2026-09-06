// thesada-fw - shell_mode_policy.h
// Which command transports a device accepts. Pure, so the parse and the two
// gates are host-testable without a serial port or a broker.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string.h>

enum ShellMode {
  SHELL_MODE_FULL = 0,   // serial + MQTT CLI (default)
  SHELL_MODE_SERIAL_ONLY,
  SHELL_MODE_MQTT_ONLY,
  SHELL_MODE_OFF,
};

// Absent or empty means a config written before the key existed: behave as
// before. A value that was typed but not understood closes instead, so a
// misspelt hardening request never silently serves the full surface.
inline ShellMode shellModeParse(const char* s) {
  if (!s || !*s) return SHELL_MODE_FULL;
  if (strcmp(s, "full") == 0)        return SHELL_MODE_FULL;
  if (strcmp(s, "serial-only") == 0) return SHELL_MODE_SERIAL_ONLY;
  if (strcmp(s, "mqtt-only") == 0)   return SHELL_MODE_MQTT_ONLY;
  return SHELL_MODE_OFF;
}

inline bool shellModeSerialAllowed(ShellMode m) {
  return m == SHELL_MODE_FULL || m == SHELL_MODE_SERIAL_ONLY;
}

inline bool shellModeMqttAllowed(ShellMode m) {
  return m == SHELL_MODE_FULL || m == SHELL_MODE_MQTT_ONLY;
}

// POST /api/cmd and the /ws/serial terminal are a third way into Shell, and
// the broadest one. Any mode but full is a hardening request, so it closes.
inline bool shellModeHttpAllowed(ShellMode m) {
  return m == SHELL_MODE_FULL;
}

// True when the parsed value did not come from a name we know, so the caller
// can say so once at boot rather than leaving the operator guessing.
inline bool shellModeUnrecognised(const char* s) {
  return s && *s && shellModeParse(s) == SHELL_MODE_OFF && strcmp(s, "off") != 0;
}
