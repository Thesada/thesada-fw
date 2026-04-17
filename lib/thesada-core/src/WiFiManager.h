// thesada-fw - WiFiManager.h
// Multi-SSID WiFi with RSSI-ranked connection, cellular fallback handoff,
// fallback AP with captive portal, and periodic WiFi re-check.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>

// Overall WiFi path state - checked by Cellular to know when to take over.
enum class WiFiStatus {
  CONNECTED,   // associated and IP assigned
  SCANNING,    // actively trying to connect
  ALL_FAILED   // every configured SSID tried and failed - cellular should start
};

class WiFiManager {
public:
  // Call once in setup(). Scans, ranks by RSSI, tries each configured SSID.
  static void       begin();

  // Call every loop(). Detects drops and re-scans while on WiFi.
  // When in AP mode, runs DNS server for captive portal.
  // AP times out after wifi.ap_timeout_s and retries WiFi scan.
  static void       loop();

  // True only when associated and IP is assigned.
  static bool       connected();

  // True when fallback AP is active.
  static bool       isAPActive();

  // Current state - Cellular polls this to decide whether to activate.
  static WiFiStatus status();

  // Called by the cellular path on its periodic schedule (default 15 min).
  // Triggers a fresh scan/connect attempt. Returns true if WiFi came back up,
  // meaning cellular should yield.
  static bool       recheckWiFi();

private:
  static void       scanAndConnect();
  static bool       tryConnect(const char* ssid, const char* password, uint32_t timeoutMs);
  static void       startFallbackAP();
  static void       stopFallbackAP();

  static WiFiStatus _status;
  static bool       _apActive;
  static uint32_t   _lastRecheck;
  static uint32_t   _recheckIntervalMs;
  static uint32_t   _apStartTime;
  static uint32_t   _apTimeoutMs;
};
