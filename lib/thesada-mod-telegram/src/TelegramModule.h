// thesada-fw - TelegramModule.h
// Telegram Bot API and webhook.url sender for Lua alert rules. send()
// also publishes the message to EventBus ("alert").
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
  const char* configKey() override { return "telegram"; }
  void status(ShellOutput out) override;

  static bool send(const char* message);
  static bool sendTo(const char* chatId, const char* message);

private:
  static bool _ready;
  // Telegram Bot API client - CA-verified against the Telegram API root
  // (see telegram_ca_progmem.h). Single instance, never deleted, avoids
  // the heap fragmentation that kills Telegram after a few alerts.
  static WiFiClientSecure _secClient;
  // PEM CA root kept live for the lifetime of _secClient: setCACert()
  // stores the pointer, not a copy, so the buffer must outlive every
  // handshake. Loaded once in begin().
  static String _caCert;
  // Separate client for the optional user-configured HTTPS webhook.
  // Verified against /webhook-ca.crt when the operator uploads one;
  // otherwise unverified - the endpoint is arbitrary (possibly
  // self-signed or internal), so it cannot be forced to chain to the
  // Telegram CA. Also single-instance.
  static WiFiClientSecure _webhookClient;
  // PEM CA for the webhook client - same pointer-lifetime contract as
  // _caCert. Empty = no /webhook-ca.crt uploaded, client stays insecure.
  static String _webhookCaCert;
};
