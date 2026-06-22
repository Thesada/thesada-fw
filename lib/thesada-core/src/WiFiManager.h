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
  // Scan, rank by RSSI, and try each configured SSID.
  // in: none. out: none. Call once in setup().
  static void       begin();

  // Detect drops and re-scan while on WiFi; run DNS server when in AP mode.
  // AP times out after wifi.ap_timeout_s and retries WiFi scan.
  // in: none. out: none. Call every loop().
  static void       loop();

  // in: none. out: true only when associated and IP is assigned.
  static bool       connected();

  // in: none. out: true when fallback AP is active.
  static bool       isAPActive();

  // in: none. out: current WiFi path state.
  static WiFiStatus status();

  // Trigger a fresh scan/connect attempt.
  // in: none. out: true if WiFi came back up (cellular should yield).
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
