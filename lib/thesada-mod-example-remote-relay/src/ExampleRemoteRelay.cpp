// thesada-fw - ExampleRemoteRelay.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include <thesada_config.h>
#include "ExampleRemoteRelay.h"
#include <Config.h>
#include <EventBus.h>
#include <MQTTClient.h>
#include <Log.h>
#include <Shell.h>
#include <ModuleRegistry.h>
#include <ArduinoJson.h>
#include <string.h>

#ifdef ENABLE_EXAMPLE_REMOTE_RELAY

static const char* TAG = "ExRelay";

// Configure the pin, drive it off, and register the one command.
// in: config block example_remote_relay (pin, active_low).
// out: `relay.set` registered on the shell.
void ExampleRemoteRelay::begin() {
  JsonObject cfg = Config::get();
  _pin       = cfg["example_remote_relay"]["pin"]        | 4;
  _activeLow = cfg["example_remote_relay"]["active_low"] | false;
  pinMode(_pin, OUTPUT);
  set(false);

  // One registration covers every transport. The shell dispatches
  // `relay.set` whether it arrived on the serial console, POST /api/cmd, or
  // the MQTT topic <prefix>/cli/relay.set - the module never sees which.
  // argv[0] is the command name, so the first argument is argv[1].
  Shell::registerCommand("relay.set", "on|off|toggle - drive the example relay",
    [this](int argc, char** argv, ShellOutput out) {
      const char* arg = (argc > 1) ? argv[1] : "";
      if      (strcmp(arg, "on") == 0)     set(true);
      else if (strcmp(arg, "off") == 0)    set(false);
      else if (strcmp(arg, "toggle") == 0) set(!_on);
      else { out("Usage: relay.set on|off|toggle"); return; }
      out(_on ? "on" : "off");
    });

  Log::kvf(TAG, "example_relay.ready pin=%d active_low=%d", _pin, (int)_activeLow);
}

// Drive the pin and announce the new state.
// in: on. out: GPIO level (honouring active_low), then publishState().
void ExampleRemoteRelay::set(bool on) {
  _on = on;
  digitalWrite(_pin, (on != _activeLow) ? HIGH : LOW);
  Log::kvf(TAG, "example_relay.set on=%d", (int)on);
  publishState();
}

// Publish after every change so a dashboard never has to poll. The event
// carries the same payload for anything on the bus (Lua rules, the display).
// in: none. out: <prefix>/sensor/relay {"on":bool} + EventBus "relay".
void ExampleRemoteRelay::publishState() {
  JsonObject  cfg    = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  char topic[96];
  snprintf(topic, sizeof(topic), "%s/sensor/relay", prefix);
  MQTTClient::publish(topic, _on ? "{\"on\":true}" : "{\"on\":false}");

  JsonDocument doc;
  doc["on"] = _on;
  EventBus::publish("relay", doc.as<JsonObject>());
}

// One line for `module.status`.
// in: out sink. out: "pin=<n> state=on|off".
void ExampleRemoteRelay::status(ShellOutput out) {
  char line[48];
  snprintf(line, sizeof(line), "pin=%d state=%s", _pin, _on ? "on" : "off");
  out(line);
}

MODULE_REGISTER(ExampleRemoteRelay, PRIORITY_OUTPUT)

#endif  // ENABLE_EXAMPLE_REMOTE_RELAY
