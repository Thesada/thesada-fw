// thesada-fw - OTAUpdate.h
// HTTP(S) pull-based OTA with manifest + SHA256 integrity check.
// Fetches a JSON manifest from a configured URL, compares version,
// downloads and verifies the binary, then flashes via Update.h.
// Triggers: periodic interval (configurable) + MQTT command.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>

class OTAUpdate {
public:
  // Call once after WiFi + MQTT are up.
  static void begin();

  // Call every loop(). Handles periodic check timing.
  static void loop();

  // Trigger an immediate OTA check. Called by MQTT handler or manual trigger.
  static void check(const char* manifestOverride = nullptr);

private:
  static bool fetchManifest(const char* url, String& version, String& binUrl, String& sha256);
  static bool applyUpdate(const String& binUrl, const String& expectedSha256);
  static bool isNewer(const char* remote, const char* local);

  static uint32_t _lastCheck;
  static uint32_t _checkIntervalMs;
  static bool     _enabled;
  static bool     _checkRequested;
  static String   _pendingManifestUrl;
};
