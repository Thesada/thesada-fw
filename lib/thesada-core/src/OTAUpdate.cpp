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
#include <esp_task_wdt.h>
#include <mbedtls/sha256.h>

static const char* TAG = "OTA";

uint32_t OTAUpdate::_lastCheck       = 0;
uint32_t OTAUpdate::_checkIntervalMs = 21600000;  // 6 hours
bool     OTAUpdate::_enabled         = true;
bool     OTAUpdate::_checkRequested  = false;
bool     OTAUpdate::_forceRequested  = false;
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

// Apply CA cert or insecure mode to a WiFiClientSecure instance.
// Short socket + handshake timeouts so a flaky upstream cannot block
// readBytes() long enough to trip the task watchdog. WiFiClientSecure
// defaults to ~60s read timeout, far past the ~5s TWDT window.
static void configureSecureClient(WiFiClientSecure& client) {
  loadCaCert();
  if (_otaCaCert.isEmpty()) {
    client.setInsecure();
  } else {
    client.setCACert(_otaCaCert.c_str());
  }
  client.setTimeout(10);            // arduino-esp32 v2.x: seconds (current platform)
  client.setHandshakeTimeout(10);   // ssl handshake, always seconds
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
  // There is NO `cli/ota.check` shell command - the only on-demand trigger
  // is publishing to the topic defined by ota.cmd_topic in config.
  // If ota.cmd_topic is set, subscribe to it.
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

// Immediate boot-time OTA check. Runs before MQTT/modules to use clean heap.
// If an update is found, the device flashes and reboots (never returns).
void OTAUpdate::checkNow() {
  if (!_enabled) return;
  if (!WiFiManager::connected()) return;
  Log::info(TAG, "Boot-time OTA check (clean heap)");
  check();
  // If we get here, no update was found. Disable the 30s delayed check
  // since we just checked.
  _lastCheck = millis();
}

// Non-blocking OTA trigger used by the Shell `ota.check` command and the
// MQTT cmd_topic callback. Setting state here lets the caller return
// immediately so CLI responses can publish before the device reboots.
void OTAUpdate::triggerCheck(const char* manifestOverride, bool force) {
  if (manifestOverride && strlen(manifestOverride) > 0) {
    _pendingManifestUrl = manifestOverride;
  } else {
    _pendingManifestUrl = "";
  }
  _forceRequested = force;
  _checkRequested = true;
}

// Periodically check for updates or handle MQTT-triggered checks
void OTAUpdate::loop() {
  // Handle deferred MQTT-triggered check (runs outside MQTT callback context).
  if (_checkRequested) {
    _checkRequested = false;
    bool force = _forceRequested;
    _forceRequested = false;
    const char* override = _pendingManifestUrl.length() > 0
                             ? _pendingManifestUrl.c_str() : nullptr;
    check(override, force);
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

// Fetch manifest, compare versions, and apply update if newer.
// `force=true` bypasses the isNewer() check so the device re-flashes whatever
// the manifest points at even if the remote version equals the local one.
// Intended for dev iteration (avoids bumping FIRMWARE_VERSION every cycle)
// and for recovering from a stuck state without a version bump.
void OTAUpdate::check(const char* manifestOverride, bool force) {
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

  Log::info(TAG, force ? "Checking for update (FORCED)..." : "Checking for update...");

  // On heap-constrained boards (CYD/WROOM), MQTT's TLS session fragments
  // heap so a second TLS connection for the manifest fetch will OOM.
  // Skip periodic checks when heap is too low - boot-time checkNow()
  // handles OTA with clean heap before MQTT connects.
  // Exception: --force means user explicitly wants to flash. Disconnect
  // MQTT to free TLS buffers, letting the manifest fetch succeed on a
  // clean heap. MQTT reconnect happens post-reboot if update applies, or
  // in loop() if we bail out.
  if (MQTTClient::connected() && ESP.getMaxAllocHeap() < 40000) {
    if (!force) {
      Log::info(TAG, "Skipping OTA check - heap too low for second TLS session");
      return;
    }
    Log::warn(TAG, "Heap low - disconnecting MQTT to free TLS buffers for forced OTA");
    MQTTClient::_client.disconnect();
    MQTTClient::_wifiClient.stop();
    delay(200);  // let TCP teardown + free mbedtls context
  }

  String remoteVersion, binUrl, sha256;
  if (!fetchManifest(url, remoteVersion, binUrl, sha256)) {
    Log::error(TAG, "Manifest fetch failed");
    return;
  }

  char msg[160];
  snprintf(msg, sizeof(msg), "Remote: %s, local: %s",
           remoteVersion.c_str(), FIRMWARE_VERSION);
  Log::info(TAG, msg);

  if (!force && !isNewer(remoteVersion.c_str(), FIRMWARE_VERSION)) {
    Log::info(TAG, "Already up to date");
    return;
  }
  if (force && !isNewer(remoteVersion.c_str(), FIRMWARE_VERSION)) {
    Log::warn(TAG, "Force flag set - re-flashing same or older version");
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
// Fetch the manifest JSON. Belt-and-braces watchdog feeding + tight timeouts
// so a stall here (TLS handshake, hung socket read on a flaky upstream)
// cannot block this task long enough to trip TWDT. configureSecureClient
// already sets 10 s socket + handshake timeouts on the local WiFiClientSecure;
// this wraps the whole fetch in the same defensive envelope as applyUpdate().
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
  http.setConnectTimeout(10000);  // ms - cap connect phase
  http.setTimeout(10000);         // ms - cap per-read wait

  yield();
  esp_task_wdt_reset();

  int code = http.GET();

  yield();
  esp_task_wdt_reset();

  if (code != 200) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Manifest HTTP %d", code);
    Log::error(TAG, msg);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  yield();
  esp_task_wdt_reset();

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

    // Feed both watchdogs every chunk. A slow readBytes() on a weak link
    // plus the per-chunk Update.write() flash time can stack past the ~5s
    // task watchdog window and panic-reboot mid-flash, leaving the new
    // partition never marked bootable.
    yield();
    esp_task_wdt_reset();

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
