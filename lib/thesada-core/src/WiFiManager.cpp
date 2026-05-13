// thesada-fw - WiFiManager.cpp
// Scans visible networks, matches against the configured "wifi.networks" list,
// ranks matches by RSSI (strongest first), and tries each for
// "wifi.timeout_per_ssid_s" seconds (default 10). On total failure sets status
// ALL_FAILED so the cellular path can take over. While on cellular,
// recheckWiFi() re-runs the scan/connect cycle every
// "wifi.wifi_check_interval_s" seconds (default 900 = 15 min).
// SPDX-License-Identifier: GPL-3.0-only

#include "WiFiManager.h"
#include "Config.h"
#include "Log.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <time.h>
#include <esp_sntp.h>

static const char* TAG = "WiFi";
static DNSServer   _dnsServer;

WiFiStatus WiFiManager::_status            = WiFiStatus::SCANNING;
bool       WiFiManager::_apActive          = false;
uint32_t   WiFiManager::_lastRecheck       = 0;
uint32_t   WiFiManager::_recheckIntervalMs = 900000UL;
uint32_t   WiFiManager::_apStartTime       = 0;
uint32_t   WiFiManager::_apTimeoutMs       = 300000UL;

// ---------------------------------------------------------------------------

// Initialize WiFi in station mode and start connection
void WiFiManager::begin() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  scanAndConnect();
}

// ---------------------------------------------------------------------------

// Scan visible networks, rank by RSSI, and connect to the best match
void WiFiManager::scanAndConnect() {
  _status = WiFiStatus::SCANNING;

  JsonObject cfg      = Config::get();
  JsonArray  networks = cfg["wifi"]["networks"].as<JsonArray>();
  uint32_t   timeout  = (uint32_t)(cfg["wifi"]["timeout_per_ssid_s"]    | 10)  * 1000UL;
  _recheckIntervalMs  = (uint32_t)(cfg["wifi"]["wifi_check_interval_s"] | 900) * 1000UL;

  if (networks.isNull() || networks.size() == 0) {
    Log::warn(TAG, "No networks configured - starting fallback AP");
    startFallbackAP();
    return;
  }

  // Passive scan of visible APs.
  Log::info(TAG, "Scanning...");
  int found = WiFi.scanNetworks();
  {
    char msg[48];
    snprintf(msg, sizeof(msg), "Scan complete: %d AP(s) visible", found);
    Log::info(TAG, msg);
  }

  // Match configured networks against scan results; record best RSSI per SSID.
  // Stack-allocated; supports up to 8 configured networks.
  struct Candidate { const char* ssid; const char* password; int32_t rssi; };
  Candidate candidates[8];
  uint8_t   count = 0;

  for (JsonObject net : networks) {
    const char* ssid = net["ssid"] | "";
    if (strlen(ssid) == 0 || count >= 8) continue;

    int32_t bestRssi = INT32_MIN;
    for (int i = 0; i < found; i++) {
      if (WiFi.SSID(i) == ssid && WiFi.RSSI(i) > bestRssi)
        bestRssi = WiFi.RSSI(i);
    }

    if (bestRssi == INT32_MIN) {
      char msg[48];
      snprintf(msg, sizeof(msg), "Not in range: %s", ssid);
      Log::debug(TAG, msg);
      continue;
    }

    candidates[count++] = { ssid, net["password"] | "", bestRssi };
  }

  WiFi.scanDelete();

  if (count == 0) {
    Log::warn(TAG, "No configured network in range");
    startFallbackAP();
    _status = WiFiStatus::ALL_FAILED;
    return;
  }

  // Sort by RSSI descending (strongest signal first).
  for (uint8_t i = 0; i < count - 1; i++) {
    for (uint8_t j = i + 1; j < count; j++) {
      if (candidates[j].rssi > candidates[i].rssi) {
        Candidate tmp  = candidates[i];
        candidates[i]  = candidates[j];
        candidates[j]  = tmp;
      }
    }
  }

  // Try each candidate, with retries. Default 2 attempts per SSID.
  uint8_t maxRetries = cfg["wifi"]["retries"] | 2;

  for (uint8_t attempt = 1; attempt <= maxRetries; attempt++) {
    for (uint8_t i = 0; i < count; i++) {
      char msg[80];
      snprintf(msg, sizeof(msg), "Trying %s (RSSI %d dBm, attempt %d/%d, timeout %lus)...",
               candidates[i].ssid, candidates[i].rssi, attempt, maxRetries, timeout / 1000UL);
      Log::info(TAG, msg);

      if (tryConnect(candidates[i].ssid, candidates[i].password, timeout)) {
        _status   = WiFiStatus::CONNECTED;
        _apActive = false;
        snprintf(msg, sizeof(msg), "Connected to %s - IP: %s",
                 candidates[i].ssid, WiFi.localIP().toString().c_str());
        Log::info(TAG, msg);

        // Start NTP sync and wait up to 15 s.
        {
          JsonObject  cfg     = Config::get();
          const char* ntpSrv  = cfg["ntp"]["server"]      | "pool.ntp.org";
          const char* ntpFb   = cfg["ntp"]["server2"]     | "time.cloudflare.com";
          int32_t     tzOff   = cfg["ntp"]["tz_offset_s"] | 0;
          configTime(tzOff, 0, ntpSrv, ntpFb);
          uint32_t t0 = millis();
          while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED &&
                 millis() - t0 < 15000) {
            delay(200);
          }
          if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            Log::info(TAG, "NTP synced");
          } else {
            Log::warn(TAG, "NTP pending - will sync in background");
          }
        }
        return;
      }

      Log::warn(TAG, "Timed out, trying next...");
    }
  }

  Log::warn(TAG, "All networks failed - starting fallback AP");
  _status = WiFiStatus::ALL_FAILED;
  startFallbackAP();
}

// ---------------------------------------------------------------------------

// Try connecting to a single SSID with a timeout
bool WiFiManager::tryConnect(const char* ssid, const char* password, uint32_t timeoutMs) {
  // Apply static IP if configured (must be done before WiFi.begin()).
  {
    JsonObject  cfg      = Config::get();
    const char* staticIp = cfg["wifi"]["static_ip"] | "";
    if (strlen(staticIp) > 0) {
      IPAddress ip, gw, sn, dns;
      ip.fromString(staticIp);
      gw.fromString(cfg["wifi"]["gateway"] | "192.168.1.1");
      sn.fromString(cfg["wifi"]["subnet"]  | "255.255.255.0");
      dns.fromString(cfg["wifi"]["dns"]    | "8.8.8.8");
      WiFi.config(ip, gw, sn, dns);
      Log::info(TAG, "Static IP configured");
    }
  }
  WiFi.begin(ssid, password);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start >= timeoutMs) {
      WiFi.disconnect(true);
      return false;
    }
    delay(250);
  }
  return true;
}

// ---------------------------------------------------------------------------

// Start a captive-portal AP for initial setup
void WiFiManager::startFallbackAP() {
  if (_apActive) return;

  JsonObject  cfg      = Config::get();
  const char* name     = cfg["device"]["name"]        | "thesada-node";
  const char* apPass   = cfg["wifi"]["ap_password"]    | "";
  _apTimeoutMs         = (uint32_t)(cfg["wifi"]["ap_timeout_s"] | 300) * 1000UL;

  char apSSID[32];
  snprintf(apSSID, sizeof(apSSID), "%s-setup", name);

  WiFi.mode(WIFI_AP);
  if (strlen(apPass) >= 8) {
    WiFi.softAP(apSSID, apPass);
  } else {
    WiFi.softAP(apSSID);
  }
  _apActive    = true;
  _apStartTime = millis();

  // Captive portal: redirect all DNS queries to AP IP.
  _dnsServer.start(53, "*", WiFi.softAPIP());

  char msg[80];
  snprintf(msg, sizeof(msg), "Fallback AP: %s (captive portal at %s, timeout %lus)",
           apSSID, WiFi.softAPIP().toString().c_str(), _apTimeoutMs / 1000UL);
  Log::warn(TAG, msg);
}

// ---------------------------------------------------------------------------

// Stop the fallback AP and return to station mode
void WiFiManager::stopFallbackAP() {
  if (!_apActive) return;
  _dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  _apActive = false;
  Log::info(TAG, "Fallback AP stopped");
}

// ---------------------------------------------------------------------------

// Return whether the fallback AP is currently running
bool WiFiManager::isAPActive() {
  return _apActive;
}

// ---------------------------------------------------------------------------

// Handle AP timeout and reconnection on WiFi loss
void WiFiManager::loop() {
  if (_apActive) {
    _dnsServer.processNextRequest();

    // AP timeout: stop AP and retry WiFi scan.
    if (_apTimeoutMs > 0 && millis() - _apStartTime >= _apTimeoutMs) {
      Log::info(TAG, "AP timeout - retrying WiFi");
      stopFallbackAP();
      scanAndConnect();
    }
    return;
  }

  // While on cellular, recheckWiFi() handles the re-scan on a timer.
  if (_status == WiFiStatus::ALL_FAILED) return;

  if (WiFi.status() != WL_CONNECTED) {
    Log::warn(TAG, "Connection lost - re-scanning");
    scanAndConnect();
  }
}

// ---------------------------------------------------------------------------

// Return whether WiFi is connected and in CONNECTED state
bool WiFiManager::connected() {
  return _status == WiFiStatus::CONNECTED && WiFi.status() == WL_CONNECTED;
}

// Return the current WiFi connection status enum
WiFiStatus WiFiManager::status() {
  return _status;
}

// ---------------------------------------------------------------------------

// Re-scan and reconnect to WiFi on a periodic timer
bool WiFiManager::recheckWiFi() {
  uint32_t now = millis();
  if (now - _lastRecheck < _recheckIntervalMs) return false;
  _lastRecheck = now;

  Log::info(TAG, "Periodic WiFi re-check...");
  scanAndConnect();
  return _status == WiFiStatus::CONNECTED;
}
