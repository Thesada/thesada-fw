// thesada-fw - OTAUpdate.cpp
// HTTP(S) pull-based OTA. Manifest -> version check -> download -> SHA256 -> flash.
//
// Manifest format (JSON at a known URL):
//   {
//     "version": "1.x",
//     "url": "https://example.com/firmware.bin",
//     "sha256": "e3b0c44298fc1c149afbf4c8996fb924..."
//   }
//
// Config keys (in config.json):
//   ota.manifest_url     - URL to the manifest JSON
//   ota.check_interval_s - seconds between periodic checks (default 21600 = 6h)
//   ota.enabled          - master enable (default true)
//   ota.cmd_topic        - MQTT topic to trigger check (empty = <prefix>/cmd/ota)
//
// TLS: uses /ca.crt from LittleFS (same file as MQTTClient and Cellular).
// If /ca.crt is not present, falls back to setInsecure().
//
// SPDX-License-Identifier: GPL-3.0-only

#include "OTAUpdate.h"
#include "Config.h"
#include "WiFiManager.h"
#include "MQTTClient.h"
#include "SleepManager.h"
#include "Log.h"
#include <thesada_config.h>

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>

static const char* TAG = "OTA";

uint32_t OTAUpdate::_lastCheck       = 0;
uint32_t OTAUpdate::_checkIntervalMs = 21600000;  // 6 hours
bool     OTAUpdate::_enabled         = true;
bool     OTAUpdate::_checkRequested  = false;
String   OTAUpdate::_pendingManifestUrl;

// Load CA cert from LittleFS, matching MQTTClient pattern.
static String _otaCaCert;
static bool   _otaCaCertLoaded = false;

static void loadCaCert() {
  if (_otaCaCertLoaded) return;
  _otaCaCertLoaded = true;
  if (LittleFS.exists("/ca.crt")) {
    File cf = LittleFS.open("/ca.crt", "r");
    if (cf) {
      _otaCaCert = cf.readString();
      cf.close();
      Log::info(TAG, "CA cert loaded from /ca.crt");
    }
  }
  if (_otaCaCert.isEmpty()) {
    Log::warn(TAG, "No /ca.crt - HTTPS will use insecure mode");
  }
}

// Apply CA cert or insecure mode to a WiFiClientSecure instance
static void configureSecureClient(WiFiClientSecure& client) {
  loadCaCert();
  if (_otaCaCert.isEmpty()) {
    client.setInsecure();
  } else {
    client.setCACert(_otaCaCert.c_str());
  }
}

// ---------------------------------------------------------------------------

// Initialize OTA from config and subscribe to MQTT trigger topic
void OTAUpdate::begin() {
  JsonObject cfg = Config::get();

  _enabled = cfg["ota"]["enabled"] | true;
  uint32_t intervalS = cfg["ota"]["check_interval_s"] | 21600;
  _checkIntervalMs = intervalS * 1000UL;

  const char* manifestUrl = cfg["ota"]["manifest_url"] | "";
  if (strlen(manifestUrl) == 0) {
    Log::warn(TAG, "No manifest_url in config - periodic OTA disabled");
    _enabled = false;
  }

  if (_enabled) {
    char msg[128];
    snprintf(msg, sizeof(msg), "Enabled - check every %lus, manifest: %.80s",
             intervalS, manifestUrl);
    Log::info(TAG, msg);
  }

  // Subscribe to MQTT OTA trigger (optional dedicated topic).
  // OTA can also be triggered via CLI: cli/ota.check
  // If ota.cmd_topic is set in config, subscribe to it for backwards compat.
  const char* customTopic = cfg["ota"]["cmd_topic"] | "";
  if (strlen(customTopic) == 0) goto skip_ota_sub;
  {
    char topic[96];
    strncpy(topic, customTopic, sizeof(topic) - 1);
    topic[sizeof(topic) - 1] = '\0';

    MQTTClient::subscribe(topic, [](const char* topic, const char* payload) {
      Log::info(TAG, "MQTT OTA trigger received");
      _checkRequested = true;
      if (payload && strlen(payload) > 8) {
        _pendingManifestUrl = payload;
      } else {
        _pendingManifestUrl = "";
      }
    });
  }
  skip_ota_sub:

  // Run first check shortly after boot (30s delay to let everything settle).
  _lastCheck = millis() - _checkIntervalMs + 30000;
}

// ---------------------------------------------------------------------------

// Periodically check for updates or handle MQTT-triggered checks
void OTAUpdate::loop() {
  // Handle deferred MQTT-triggered check (runs outside MQTT callback context).
  if (_checkRequested) {
    _checkRequested = false;
    const char* override = _pendingManifestUrl.length() > 0
                             ? _pendingManifestUrl.c_str() : nullptr;
    check(override);
    _pendingManifestUrl = "";
    return;
  }

  if (!_enabled) return;
  if (!WiFiManager::connected()) return;

  // When deep sleep is enabled, millis() resets every wake cycle.
  // Use wall clock (NTP) + RTC-persisted last check time instead.
  if (SleepManager::enabled()) {
    time_t now = time(nullptr);
    if (now > 1700000000) {  // NTP synced
      time_t last = SleepManager::lastOtaCheck();
      if (last == 0 || (now - last) >= (time_t)(_checkIntervalMs / 1000)) {
        SleepManager::setLastOtaCheck(now);
        check(nullptr);
      }
    }
    return;
  }

  uint32_t now = millis();
  if (now - _lastCheck >= _checkIntervalMs) {
    _lastCheck = now;
    check(nullptr);
  }
}

// ---------------------------------------------------------------------------

// Fetch manifest, compare versions, and apply update if newer
void OTAUpdate::check(const char* manifestOverride) {
  if (!WiFiManager::connected()) {
    Log::warn(TAG, "No WiFi - skipping OTA check");
    return;
  }

  JsonObject cfg = Config::get();
  const char* url = manifestOverride
                      ? manifestOverride
                      : (cfg["ota"]["manifest_url"] | "");

  if (strlen(url) == 0) {
    Log::error(TAG, "No manifest URL");
    return;
  }

  Log::info(TAG, "Checking for update...");

  String remoteVersion, binUrl, sha256;
  if (!fetchManifest(url, remoteVersion, binUrl, sha256)) {
    Log::error(TAG, "Manifest fetch failed");
    return;
  }

  char msg[160];
  snprintf(msg, sizeof(msg), "Remote: %s, local: %s",
           remoteVersion.c_str(), FIRMWARE_VERSION);
  Log::info(TAG, msg);

  if (!isNewer(remoteVersion.c_str(), FIRMWARE_VERSION)) {
    Log::info(TAG, "Already up to date");
    return;
  }

  snprintf(msg, sizeof(msg), "Updating to %s from %.80s",
           remoteVersion.c_str(), binUrl.c_str());
  Log::info(TAG, msg);

  // Publish status via MQTT before starting.
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  char statusTopic[96];
  snprintf(statusTopic, sizeof(statusTopic), "%s/status/ota", prefix);
  char statusPayload[128];
  snprintf(statusPayload, sizeof(statusPayload),
           "{\"state\":\"updating\",\"from\":\"%s\",\"to\":\"%s\"}",
           FIRMWARE_VERSION, remoteVersion.c_str());
  MQTTClient::publish(statusTopic, statusPayload);
  delay(100);

  if (applyUpdate(binUrl, sha256)) {
    Log::info(TAG, "Update applied - restarting");
    delay(500);
    ESP.restart();
  } else {
    Log::error(TAG, "Update failed");
    snprintf(statusPayload, sizeof(statusPayload),
             "{\"state\":\"failed\",\"version\":\"%s\"}", FIRMWARE_VERSION);
    MQTTClient::publish(statusTopic, statusPayload);
  }
}

// ---------------------------------------------------------------------------

// Download and parse the OTA manifest JSON from a URL
bool OTAUpdate::fetchManifest(const char* url,
                               String& version, String& binUrl, String& sha256) {
  HTTPClient http;
  WiFiClientSecure secureClient;
  WiFiClient plainClient;

  bool https = strncmp(url, "https://", 8) == 0;

  if (https) {
    configureSecureClient(secureClient);
    http.begin(secureClient, url);
  } else {
    http.begin(plainClient, url);
  }

  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  int code = http.GET();
  if (code != 200) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Manifest HTTP %d", code);
    Log::error(TAG, msg);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Log::error(TAG, "Manifest JSON parse failed");
    return false;
  }

  const char* v = doc["version"] | "";
  const char* u = doc["url"]     | "";
  const char* s = doc["sha256"]  | "";

  if (strlen(v) == 0 || strlen(u) == 0 || strlen(s) == 0) {
    Log::error(TAG, "Manifest missing required fields (version, url, sha256)");
    return false;
  }

  version = v;
  binUrl  = u;
  sha256  = s;
  return true;
}

// ---------------------------------------------------------------------------

// Download firmware binary, verify SHA256, and flash it
bool OTAUpdate::applyUpdate(const String& binUrl, const String& expectedSha256) {
  HTTPClient http;
  WiFiClientSecure secureClient;
  WiFiClient plainClient;

  bool https = binUrl.startsWith("https://");

  if (https) {
    configureSecureClient(secureClient);
    http.begin(secureClient, binUrl);
  } else {
    http.begin(plainClient, binUrl);
  }

  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(30000);

  int code = http.GET();
  if (code != 200) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Binary HTTP %d", code);
    Log::error(TAG, msg);
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    Log::error(TAG, "Unknown content length - cannot proceed");
    http.end();
    return false;
  }

  char msg[80];
  snprintf(msg, sizeof(msg), "Downloading %d bytes", contentLength);
  Log::info(TAG, msg);

  if (!Update.begin(contentLength)) {
    Log::error(TAG, "Update.begin() failed - not enough space?");
    http.end();
    return false;
  }

  // Streaming download with incremental SHA256.
  mbedtls_sha256_context sha_ctx;
  mbedtls_sha256_init(&sha_ctx);
  mbedtls_sha256_starts(&sha_ctx, 0);  // 0 = SHA-256

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  int remaining = contentLength;
  int written = 0;

  while (remaining > 0) {
    int toRead = min((int)sizeof(buf), remaining);
    int bytesRead = stream->readBytes(buf, toRead);

    if (bytesRead <= 0) {
      Log::error(TAG, "Download stream interrupted");
      mbedtls_sha256_free(&sha_ctx);
      Update.abort();
      http.end();
      return false;
    }

    size_t bytesWritten = Update.write(buf, bytesRead);
    if (bytesWritten != (size_t)bytesRead) {
      Log::error(TAG, "Update.write() short write");
      mbedtls_sha256_free(&sha_ctx);
      Update.abort();
      http.end();
      return false;
    }

    mbedtls_sha256_update(&sha_ctx, buf, bytesRead);

    remaining -= bytesRead;
    written += bytesRead;

    if ((written % 65536) < 1024) {
      snprintf(msg, sizeof(msg), "Progress: %d / %d bytes (%d%%)",
               written, contentLength, (written * 100) / contentLength);
      Log::info(TAG, msg);
    }
  }

  http.end();

  // Finalize SHA256.
  uint8_t hash[32];
  mbedtls_sha256_finish(&sha_ctx, hash);
  mbedtls_sha256_free(&sha_ctx);

  char computedHex[65];
  for (int i = 0; i < 32; i++) {
    sprintf(computedHex + (i * 2), "%02x", hash[i]);
  }
  computedHex[64] = '\0';

  if (strcasecmp(computedHex, expectedSha256.c_str()) != 0) {
    Log::error(TAG, "SHA256 mismatch!");
    Log::error(TAG, computedHex);
    Update.abort();
    return false;
  }

  Log::info(TAG, "SHA256 verified");

  if (!Update.end(true)) {
    Log::error(TAG, "Update.end() failed");
    return false;
  }

  return true;
}

// ---------------------------------------------------------------------------

// Compare semver strings to determine if remote is newer
bool OTAUpdate::isNewer(const char* remote, const char* local) {
  int rMajor = 0, rMinor = 0, rPatch = 0;
  int lMajor = 0, lMinor = 0, lPatch = 0;

  sscanf(remote, "%d.%d.%d", &rMajor, &rMinor, &rPatch);
  sscanf(local,  "%d.%d.%d", &lMajor, &lMinor, &lPatch);

  if (rMajor != lMajor) return rMajor > lMajor;
  if (rMinor != lMinor) return rMinor > lMinor;
  return rPatch > lPatch;
}
