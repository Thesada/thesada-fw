// thesada-fw - EthModule.h
// LAN8720A Ethernet support for WT32-ETH01 and similar boards.
// Replaces WiFiManager as the network transport when ENABLE_ETH is set.
// MQTT, OTA, Telegram, and all network features work over Ethernet.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <Module.h>

class EthModule : public Module {
public:
  void begin() override;
  void loop() override;
  const char* name() override { return "Ethernet"; }
  void status(ShellOutput out) override;
  void selftest(ShellOutput out) override;

  static bool connected();
  static String localIP();
  static void earlyInit();  // called from main.cpp before WiFiManager

  // Public so the event handler (non-member function) can set it
  static bool _connected;
};
