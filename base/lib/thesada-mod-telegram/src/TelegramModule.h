// thesada-fw - TelegramModule.h
// Alert module: evaluates temperature thresholds and low-battery rules.
// On alert, publishes to EventBus ("alert") and optionally sends directly
// via Telegram Bot API if telegram.bot_token and telegram.chat_ids are
// set in config.json. Also supports webhook.url.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <Module.h>

class TelegramModule : public Module {
public:
  void begin() override;
  void loop() override;
  const char* name() override { return "TelegramModule"; }
  void status(ShellOutput out) override;

  static bool send(const char* message);
  static bool sendTo(const char* chatId, const char* message);

private:
  static bool _ready;
  static WiFiClientSecure _secClient;  // single instance, never deleted, avoids heap fragmentation
};
