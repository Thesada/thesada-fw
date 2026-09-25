// thesada-fw - ExampleRemoteRelay.h
// Starter module: a GPIO that an inbound command flips. The actuator shape -
// no timer, one shell command that is reachable over serial, HTTP and MQTT
// alike, and a state publish after every change.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <Module.h>

class ExampleRemoteRelay : public Module {
public:
  void begin() override;
  void loop() override {}
  const char* name() override { return "ExampleRemoteRelay"; }
  const char* configKey() override { return "example_remote_relay"; }
  void status(ShellOutput out) override;

private:
  void set(bool on);
  void publishState();

  int  _pin       = 4;
  bool _activeLow = false;
  bool _on        = false;
};
