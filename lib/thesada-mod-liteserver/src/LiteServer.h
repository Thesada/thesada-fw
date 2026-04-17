// thesada-fw - LiteServer.h
// Lightweight HTTP server for heap-constrained boards (CYD, WROOM without PSRAM).
// Uses Arduino built-in WebServer (blocking, single-client, ~3-5KB RAM).
// Provides: push OTA, config editor, WiFi AP provisioning with captive portal.
// Not a replacement for HttpServer - a companion for boards where ENABLE_WEBSERVER is off.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>

class LiteServer {
public:
  static void begin();
  static void loop();
};
