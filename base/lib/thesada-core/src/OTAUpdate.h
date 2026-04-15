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
  // `force=true` bypasses isNewer() so the device re-flashes whatever the
  // manifest points at even when the remote version equals the local one.
  // Intended for dev iteration (no version-bump-every-test-cycle) and for
  // recovering a stuck device without having to bump FIRMWARE_VERSION.
  //
  // WARNING: this is synchronous - it blocks on manifest fetch + download +
  // flash + ESP.restart(). Callers running inside the MQTT loop (e.g. the
  // Shell `ota.check` command) must use `triggerCheck()` below instead,
  // otherwise the CLI response never publishes because the device reboots
  // before Shell::execute can return.
  static void check(const char* manifestOverride = nullptr, bool force = false);

  // Non-blocking trigger. Sets the deferred state so the next OTAUpdate::loop()
  // tick picks up the request and runs check() synchronously from the main
  // loop context. Safe to call from Shell command handlers (lets the CLI
  // response publish before the device reboots).
  static void triggerCheck(const char* manifestOverride = nullptr, bool force = false);

private:
  static bool fetchManifest(const char* url, String& version, String& binUrl, String& sha256);
  static bool applyUpdate(const String& binUrl, const String& expectedSha256);
  static bool isNewer(const char* remote, const char* local);

  static uint32_t _lastCheck;
  static uint32_t _checkIntervalMs;
  static bool     _enabled;
  static bool     _checkRequested;
  static bool     _forceRequested;
  static String   _pendingManifestUrl;
};
