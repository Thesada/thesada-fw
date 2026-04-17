// thesada-fw - HttpServer.h
// Async HTTP server. Always active when WiFi or AP is up.
// Routes:
//   GET  /              - live sensor dashboard (HTML, auth required)
//   GET  /api/info      - firmware version + build info (no auth)
//   GET  /api/state     - latest sensor readings as JSON (auth required)
//   GET  /api/config    - current config.json (auth required)
//   POST /api/config    - write new config.json, restart (auth required)
//   POST /api/backup    - copy config.json to SD card (auth required)
//   POST /api/restart   - reboot device (auth required)
//   POST /ota           - accept firmware binary upload (auth required)
//   WS   /ws/serial     - bidirectional serial terminal (no auth, local only)
// Write endpoints require HTTP Basic Auth (credentials from config.json).
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

class HttpServer {
public:
  // Call once after WiFi/AP is up.
  static void begin();

  // Call every loop() - handles deferred restarts.
  static void loop();

  // Called by EventBus subscribers to push fresh data into the state cache.
  static void updateState(const char* key, JsonObject data);

  // Called by Log::_remoteHandler to relay log lines to WebSocket clients.
  static void broadcastLog(const char* line);

  // Print current sensor state JSON to Serial (and WS clients via Log).
  static void printState();

private:
  static void setupRoutes();
  static void subscribeToEvents();
};
