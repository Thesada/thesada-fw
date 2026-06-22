// thesada-fw - OTAUpdate.h
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>

class OTAUpdate {
public:
  // in:  none
  // out: none
  static void begin();

  // in:  none
  // out: none
  static void loop();

  // Fetch manifest, compare version, download, verify SHA256, flash, reboot.
  // force=true bypasses isNewer() - re-flashes even when remote == local.
  // Useful for dev iteration without bumping FIRMWARE_VERSION each cycle.
  //
  // WARNING: synchronous - blocks on fetch + download + flash + ESP.restart().
  // Callers inside the MQTT/Shell loop must use triggerCheck() instead;
  // calling check() directly means the CLI response never publishes because
  // the device reboots before Shell::execute() can return.
  //
  // in:  manifestOverride - alternate manifest URL, or nullptr for configured URL
  //      force            - re-flash even when versions match
  // out: does not return on success (reboots); returns on no-update or error
  static void check(const char* manifestOverride = nullptr, bool force = false);

  // Deferred check: safe to call from Shell command handlers.
  // Schedules check() to run from the main loop context, so the CLI response
  // publishes before the device reboots.
  //
  // in:  manifestOverride - alternate manifest URL, or nullptr for configured URL
  //      force            - re-flash even when versions match
  // out: none
  static void triggerCheck(const char* manifestOverride = nullptr, bool force = false);

  // Boot-time check before MQTT/modules load, while heap is still contiguous.
  // If update found, flashes and reboots; otherwise returns immediately.
  //
  // in:  none
  // out: does not return on success (reboots); returns on no-update, error, or OTA disabled
  static void checkNow();

private:
  static bool fetchManifest(const char* url, String& version, String& binUrl, String& sha256, size_t& size);
  static bool applyUpdate(const String& binUrl, const String& expectedSha256, size_t expectedSize);
  static bool isNewer(const char* remote, const char* local);

  static uint32_t _lastCheck;
  static uint32_t _checkIntervalMs;
  static bool     _enabled;
  static bool     _checkRequested;
  static bool     _forceRequested;
  static String   _pendingManifestUrl;
};
