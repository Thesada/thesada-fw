// thesada-fw - OTAUpdate.h
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>

class OTAUpdate {
public:
  // in:  none
  // out: none
  static void begin();

  // Register the optional ota.cmd_topic MQTT subscription. begin() calls it;
  // MQTTClient::begin() calls it again after resetting its table.
  static void registerCommandTopic();

  // in:  none
  // out: none
  static void loop();

  // Remember that a broker session came up this boot. The health gate marks
  // a pending image valid on this, or after five minutes up. in: none. out: none.
  static void noteMqttUp();

  // Record this boot in the crash streak and the brownout counter.
  // One NVS "boot" session updates crash_n and brownout_n.
  // in: esp_reset_reason value, brownout total out (may be null).
  // out: streak after this boot. On an unread streak the return is the
  // crash limit and confirm stays shut.
  static uint32_t recordBootStreak(int resetReason, uint32_t* brownoutsOut);

  // If the streak is at the limit, boot the other valid image.
  // in: none. out: none. Does not return when the switch works.
  static void rollbackIfCrashLoop();

  // Confirm a pending image, or leave it pending. Call every main-loop turn,
  // including when loop() is skipped. in: none. out: none.
  static void confirmIfHealthy();

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
