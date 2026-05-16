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
#include "Shell.h"
#include "ota_ca_progmem.h"
#include <thesada_config.h>

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <functional>
#include <mbedtls/sha256.h>

#ifdef ENABLE_CELLULAR
#include "Cellular.h"
#endif

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
    // Fallback to baked PROGMEM bundle. Prevents silent OTA disable when
    // LittleFS is fresh/wiped (firmware-only reflash without uploadfs).
    // FPSTR + assignment copies flash->RAM as a one-shot at boot; OK
    // because _otaCaCert is then reused for every subsequent fetch.
    _otaCaCert = String(FPSTR(OTA_CA_PROGMEM));
    if (!_otaCaCert.isEmpty()) {
      Log::warn(TAG, "No /ca.crt in LittleFS - using baked PROGMEM CA bundle");
    } else {
      Log::error(TAG, "No /ca.crt AND PROGMEM bundle empty - HTTPS will use insecure mode");
    }
  }
}

// Publish a status/ota refusal record so operators see WHY a check did
// not proceed without needing serial. Without this, every silent
// bailout in check() / begin() looks identical to "device offline" or
// "manifest unreachable" from the broker side. Best-effort: a publish
// failure is itself silently dropped (we are already in a failure path).
// in:  reason (short kebab-case identifier, e.g. "no-ca", "heap-low").
// out: none.
static void publishOtaRefusal(const char* reason) {
  if (!MQTTClient::connected()) return;
  JsonObject  cfg    = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  char topic[96];
  snprintf(topic, sizeof(topic), "%s/status/ota", prefix);
  char payload[160];
  snprintf(payload, sizeof(payload),
           "{\"state\":\"refused\",\"reason\":\"%s\",\"version\":\"%s\"}",
           reason, FIRMWARE_VERSION);
  MQTTClient::publish(topic, payload);
}

// Apply CA cert or insecure mode to a WiFiClientSecure instance.
// Short socket + handshake timeouts so a flaky upstream cannot block
// readBytes() long enough to trip the task watchdog. WiFiClientSecure
// defaults to ~60s read timeout, far past the ~5s TWDT window.
//
// When /ca.crt is missing OTAUpdate::begin() refuses to enable OTA unless
// ota.allow_insecure is set in config.json, so by the time we get here we
// already know either the CA cert is loaded or the operator has opted in.
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
// Transport-agnostic HTTP GET
//
// OTA fallback over cellular when WiFi is down. The two
// underlying paths are very different (Arduino HTTPClient/WiFiClientSecure
// over WiFi vs SIM7080G modem-native SSL socket via TinyGsmClientSecure),
// so we wrap both behind a single byte-stream callback API. Same /ca.crt
// validates the manifest origin on either path (public TLS roots covering
// the common backends - GitHub Releases, Let's Encrypt fronted origins,
// Sectigo, etc - are baked into the firmware via ota_ca_progmem.h).
// ---------------------------------------------------------------------------

// HTTP GET via WiFi using Arduino HTTPClient. Streams the response body
// through writeCallback; if the callback returns false the transfer is
// aborted. Returns true if the HTTP transport completed (any status
// code); the caller checks *outStatus.
// in:  url, writeCallback. out: outStatus (200/404/...), outLen (bytes).
static bool otaHttpGetWiFi(const char* url,
                           std::function<bool(const uint8_t*, size_t)> writeCallback,
                           int* outStatus, size_t* outLen) {
  HTTPClient http;
  WiFiClientSecure secureClient;
  WiFiClient       plainClient;
  bool https = strncmp(url, "https://", 8) == 0;
  if (https) {
    configureSecureClient(secureClient);
    http.begin(secureClient, url);
  } else {
    http.begin(plainClient, url);
  }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setConnectTimeout(10000);
  http.setTimeout(10000);

  yield(); esp_task_wdt_reset();
  int code = http.GET();
  yield(); esp_task_wdt_reset();
  if (outStatus) *outStatus = code;
  if (code != 200) {
    http.end();
    return code > 0;  // negative = transport error; positive = HTTP-level error (still "transport ok")
  }

  WiFiClient* stream = http.getStreamPtr();
  int contentLength  = http.getSize();
  size_t total = 0;
  uint8_t buf[1024];
  uint32_t deadline = millis() + 5UL * 60UL * 1000UL;  // 5 min cap
  while (millis() < deadline) {
    yield(); esp_task_wdt_reset();
    Shell::pumpConsole();
    if (!stream->connected() && !stream->available()) break;
    int avail = stream->available();
    if (avail <= 0) { delay(10); continue; }
    int n = stream->readBytes(buf, std::min((size_t)avail, sizeof(buf)));
    if (n <= 0) break;
    total += n;
    if (writeCallback && !writeCallback(buf, n)) {
      http.end();
      if (outLen) *outLen = total;
      return false;
    }
    if (contentLength > 0 && (int)total >= contentLength) break;
  }
  http.end();
  if (outLen) *outLen = total;
  return true;
}

#ifdef ENABLE_CELLULAR
// HTTP GET via SIM7080G modem-native SSL socket (Cellular::httpsGet ->
// TinyGsmClientSecure -> CAOPEN/CASEND/CARECV). Parses URL into host/
// path/port; the lower-level helper handles status-line + headers + body
// chunking. Same writeCallback signature as the WiFi path.
// in:  url, writeCallback. out: outStatus, outLen.
static bool otaHttpGetCellular(const char* url,
                               std::function<bool(const uint8_t*, size_t)> writeCallback,
                               int* outStatus, size_t* outLen) {
  const char* p = url;
  uint16_t port = 443;
  if (strncmp(p, "https://", 8) == 0)      { p += 8; port = 443; }
  else if (strncmp(p, "http://",  7) == 0) { p += 7; port = 80;  }
  char host[128];
  const char* slash   = strchr(p, '/');
  const char* hostEnd = slash ? slash : p + strlen(p);
  size_t hostLen      = hostEnd - p;
  if (hostLen == 0 || hostLen >= sizeof(host)) return false;
  memcpy(host, p, hostLen);
  host[hostLen] = '\0';
  char* colon = strchr(host, ':');
  if (colon) { *colon = '\0'; port = atoi(colon + 1); }
  const char* path = slash ? slash : "/";

  size_t total = 0;
  auto wrap = [&](const uint8_t* buf, size_t len) -> bool {
    total += len;
    return writeCallback ? writeCallback(buf, len) : true;
  };
  bool ok = Cellular::httpsGet(host, path, port, wrap, outStatus);
  if (outLen) *outLen = total;
  return ok;
}
#endif

#ifdef ENABLE_CELLULAR
// Parse `host`, `path`, `port` out of an absolute http(s) URL. Returns
// false on malformed input or oversized host string.
static bool parseUrl(const char* url, char* host, size_t hostCap,
                     const char** pathOut, uint16_t* portOut) {
  const char* p = url;
  uint16_t port = 443;
  if (strncmp(p, "https://", 8) == 0)      { p += 8; port = 443; }
  else if (strncmp(p, "http://",  7) == 0) { p += 7; port = 80;  }
  else return false;
  const char* slash   = strchr(p, '/');
  const char* hostEnd = slash ? slash : p + strlen(p);
  size_t hostLen      = hostEnd - p;
  if (hostLen == 0 || hostLen >= hostCap) return false;
  memcpy(host, p, hostLen);
  host[hostLen] = '\0';
  char* colon = strchr(host, ':');
  if (colon) { *colon = '\0'; port = atoi(colon + 1); }
  *pathOut = slash ? slash : "/";
  *portOut = port;
  return true;
}

// Download a binary in 64 KB chunks via HTTP Range requests. Each chunk
// is its own short-lived TLS session, which works around SIM7080G HTTP
// session degradation past ~500-900 KB (observed on fw 1951B17). The
// WiFi transport does not need this; HTTPClient handles 1+ MB downloads
// in one socket fine.
//
// expectedSize MUST be > 0 - it is the loop's termination condition.
// Comes from the manifest's `size` field; without it this function
// refuses to run and the caller falls back to a single GET.
static bool downloadBinaryChunkedCellular(
    const char* url, size_t expectedSize,
    std::function<bool(const uint8_t*, size_t)> writeCallback,
    size_t* totalOut) {
  if (totalOut) *totalOut = 0;
  if (expectedSize == 0) return false;

  char host[128];
  const char* path = nullptr;
  uint16_t port = 443;
  if (!parseUrl(url, host, sizeof(host), &path, &port)) {
    Log::error(TAG, "downloadBinaryChunked: bad URL");
    return false;
  }

  // MQTT (SMCONN) runs inside the modem fw and competes with our HTTPS
  // chunks for the modem's TCP socket slots and the PDP context's
  // bandwidth, even though our ATGuard blocks our own SMPUB traffic.
  // Drop the modem-native MQTT session before the download so the chunks
  // get the full bandwidth budget. Cellular::loop() detects the dropped
  // session on its next tick after ATGuard releases and re-establishes
  // MQTT automatically (existing recovery path), so no explicit SMCONN
  // is needed here. On OTA success the device reboots and MQTT comes up
  // fresh on the new fw anyway.
  Cellular::atPassthrough("+SMDISC", 5000UL, [](const char*){});

  // 64 KB chunk size. At ~8.4 KB/s LTE-M that is ~8 s per chunk, well
  // under the empirically observed session-degradation threshold. For
  // a 1.48 MB binary this is ~23 round trips.
  static constexpr size_t   CHUNK             = 64UL * 1024UL;
  // Inter-chunk pacing: lets the modem's internal TCP slots clear
  // TIME_WAIT and the carrier's per-source-port rate-counter decay
  // before the next handshake. Bench-tuned on LTE-M; trimmable if a
  // future carrier handles back-to-back sockets cleanly.
  static constexpr uint32_t CHUNK_GAP_MS      = 5000UL;
  // Reset the PDP context every Nth chunk as a heavier escape hatch
  // for the same problem class (modem-internal state accumulation).
  // 8 chunks * 64 KB = 512 KB which is where the empirical stall
  // showed up most consistently.
  static constexpr int      CHUNK_BOUNCE_EVERY = 8;

  size_t offset    = 0;
  int    chunkIdx  = 0;
  while (offset < expectedSize) {
    size_t end = std::min(offset + CHUNK - 1, expectedSize - 1);
    int status = 0;
    bool ok = Cellular::httpsGet(host, path, port, writeCallback, &status,
                                 offset, end);
    if (!ok || (status != 206 && status != 200)) {
      char msg[160];
      snprintf(msg, sizeof(msg),
               "Range chunk failed at offset %u (status=%d, ok=%d)",
               (unsigned)offset, status, (int)ok);
      Log::error(TAG, msg);
      return false;
    }
    offset = end + 1;
    chunkIdx++;
    if (totalOut) *totalOut = offset;

    if (offset >= expectedSize) break;

    // Periodic CNACT bounce. Deactivate + reactivate the PDP context
    // so the modem releases every TCP slot still in TIME_WAIT and the
    // operator-side flow counters reset. Cheap (~1-2 s) compared to
    // the recovery cost when a chunk eventually stalls without it.
    if (chunkIdx % CHUNK_BOUNCE_EVERY == 0) {
      Log::info(TAG, "Bouncing PDP context (CNACT cycle)");
      Cellular::atPassthrough("+CNACT=0,0", 5000UL, [](const char*){});
      delay(500);
      Cellular::atPassthrough("+CNACT=0,1", 10000UL, [](const char*){});
      // CNACT activation completes asynchronously via +APP PDP URC;
      // a short settle window before the next handshake is reliable.
      for (int i = 0; i < 20; ++i) {
        Shell::pumpConsole();
        delay(100);
        esp_task_wdt_reset();
      }
    }

    // Inter-chunk pacing while feeding the watchdog.
    uint32_t paceUntil = millis() + CHUNK_GAP_MS;
    while (millis() < paceUntil) {
      Shell::pumpConsole();
      esp_task_wdt_reset();
      delay(200);
    }
  }
  return true;
}
#endif // ENABLE_CELLULAR

// Pick whichever transport is currently up. WiFi wins when both are up
// (cheaper than waking + holding the cellular TX). Caller MUST already
// have ensured at least one transport is connected (the OTAUpdate loop
// gates on this).
// in:  url, writeCallback. out: outStatus, outLen.
static bool otaHttpGet(const char* url,
                      std::function<bool(const uint8_t*, size_t)> writeCallback,
                      int* outStatus, size_t* outLen) {
  if (outStatus) *outStatus = 0;
  if (outLen)    *outLen    = 0;
  if (WiFiManager::connected()) {
    return otaHttpGetWiFi(url, writeCallback, outStatus, outLen);
  }
#ifdef ENABLE_CELLULAR
  if (Cellular::connected()) {
    return otaHttpGetCellular(url, writeCallback, outStatus, outLen);
  }
#endif
  Log::warn(TAG, "otaHttpGet: no transport available");
  return false;
}

// True when any OTA-capable transport is up.
static bool anyTransportUp() {
  if (WiFiManager::connected()) return true;
#ifdef ENABLE_CELLULAR
  if (Cellular::connected())    return true;
#endif
  return false;
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

  // Refuse to bring OTA up without a CA cert unless ota.allow_insecure is
  // explicitly set in config.json. With no cert, both manifest and binary
  // come over an unverified HTTPS channel - the SHA256 in the manifest is
  // not MITM protection because an attacker who can MITM controls both.
  // Dev workflows that legitimately need to bypass set the opt-in.
  if (_enabled) {
    loadCaCert();
    bool allowInsecure = cfg["ota"]["allow_insecure"] | false;
    if (_otaCaCert.isEmpty() && !allowInsecure) {
      Log::error(TAG, "OTA disabled - no /ca.crt and ota.allow_insecure not set");
      _enabled = false;
    } else if (_otaCaCert.isEmpty() && allowInsecure) {
      Log::warn(TAG, "OTA: ota.allow_insecure=true - HTTPS without cert verification");
    }
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
  if (!anyTransportUp()) return;
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
  if (!anyTransportUp()) return;

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
  if (!anyTransportUp()) {
    Log::warn(TAG, "No transport - skipping OTA check");
    publishOtaRefusal("no-transport");
    return;
  }

  JsonObject cfg = Config::get();
  const char* url = manifestOverride
                      ? manifestOverride
                      : (cfg["ota"]["manifest_url"] | "");

  if (strlen(url) == 0) {
    Log::error(TAG, "No manifest URL");
    publishOtaRefusal("no-manifest-url");
    return;
  }

  // Mirror the begin()-time refusal so a triggered check (cli/ota.check or
  // cmd_topic publish) cannot bypass it. begin() may have set _enabled=false,
  // but triggerCheck() -> loop() runs check() regardless of _enabled to allow
  // operator-driven retries; without this gate, an attacker who can publish
  // to the OTA cmd_topic could still force an insecure HTTPS fetch even
  // though begin() refused at boot.
  //
  // With the PROGMEM CA fallback in loadCaCert this gate effectively only
  // fires when the PROGMEM bundle itself is empty (build misconfig). Kept
  // anyway so a future allow_insecure code path stays consistent.
  loadCaCert();
  bool allowInsecure = cfg["ota"]["allow_insecure"] | false;
  if (_otaCaCert.isEmpty() && !allowInsecure) {
    Log::error(TAG, "OTA check refused - no /ca.crt and ota.allow_insecure not set");
    publishOtaRefusal("no-ca");
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
  // Only applies to the WiFi transport: cellular MQTT TLS runs inside
  // the modem so it does not fragment ESP heap, and the manifest fetch
  // goes through the modem's separate SSL socket layer regardless.
  if (WiFiManager::connected() &&
      MQTTClient::connected() && ESP.getMaxAllocHeap() < 40000) {
    if (!force) {
      Log::info(TAG, "Skipping OTA check - heap too low for second TLS session");
      publishOtaRefusal("heap-low");
      return;
    }
    Log::warn(TAG, "Heap low - disconnecting MQTT to free TLS buffers for forced OTA");
    MQTTClient::_client.disconnect();
    MQTTClient::_wifiClient.stop();
    delay(200);  // let TCP teardown + free mbedtls context
  }

  String remoteVersion, binUrl, sha256;
  size_t binSize = 0;
  if (!fetchManifest(url, remoteVersion, binUrl, sha256, binSize)) {
    Log::error(TAG, "Manifest fetch failed");
    publishOtaRefusal("manifest-fetch-failed");
    return;
  }

  char msg[160];
  snprintf(msg, sizeof(msg), "Remote: %s, local: %s",
           remoteVersion.c_str(), FIRMWARE_VERSION);
  Log::info(TAG, msg);

  if (!force && !isNewer(remoteVersion.c_str(), FIRMWARE_VERSION)) {
    Log::info(TAG, "Already up to date");
    // Publish so operators see the check completed and the device is on
    // the latest. Without this an up-to-date check looks identical to
    // "device offline" or "silent refusal" from the broker side.
    const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/status/ota", prefix);
    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"state\":\"up-to-date\",\"version\":\"%s\",\"remote\":\"%s\"}",
             FIRMWARE_VERSION, remoteVersion.c_str());
    MQTTClient::publish(topic, payload);
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

  if (applyUpdate(binUrl, sha256, binSize)) {
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
// cannot block this task long enough to trip TWDT. Goes through the
// unified otaHttpGet helper so the same call site works over WiFi or
// the SIM7080G modem-native SSL socket.
bool OTAUpdate::fetchManifest(const char* url,
                               String& version, String& binUrl, String& sha256,
                               size_t& size) {
  size = 0;
  String body;
  // Cap the manifest at 8 KB - real manifests are ~200 B; anything
  // larger is either a misconfig or a malicious upstream.
  static constexpr size_t kManifestCap = 8 * 1024;
  auto cb = [&](const uint8_t* buf, size_t len) -> bool {
    if (body.length() + len > kManifestCap) return false;
    body.concat(reinterpret_cast<const char*>(buf), len);
    return true;
  };

  int    status = 0;
  size_t total  = 0;
  bool transportOk = otaHttpGet(url, cb, &status, &total);

  if (!transportOk || status != 200) {
    char msg[80];
    snprintf(msg, sizeof(msg), "Manifest fetch failed (status=%d, bytes=%u)",
             status, (unsigned)total);
    Log::error(TAG, msg);
    return false;
  }

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
  // size: optional. Required for the cellular chunked Range path; the
  // WiFi path falls back to Content-Length from the binary GET if absent.
  size_t      sz = doc["size"]   | (size_t)0;

  if (strlen(v) == 0 || strlen(u) == 0 || strlen(s) == 0) {
    Log::error(TAG, "Manifest missing required fields (version, url, sha256)");
    return false;
  }

  version = v;
  binUrl  = u;
  sha256  = s;
  size    = sz;
  return true;
}

// ---------------------------------------------------------------------------

// Download firmware binary, verify SHA256, and flash it.
//
// WiFi transport: single GET through otaHttpGet (HTTPClient handles
// multi-MB downloads in one socket without session degradation).
// Cellular transport: 64 KB Range chunks via downloadBinaryChunkedCellular.
// Each chunk is its own short-lived TLS session, defeating the
// SIM7080G HTTP-session degradation observed past ~500-900 KB on fw
// 1951B17. Requires manifest `size` field; missing -> single-GET
// fallback with a warn (may stall on cellular).
//
// Both paths feed the same streaming callback (Update.write +
// mbedtls_sha256_update) so the binary is never buffered in heap.
//
// Update.begin(UPDATE_SIZE_UNKNOWN) keeps the streaming path agnostic
// of whether Content-Length was present on the wire. Trade-off: Update
// can't pre-check "fits in OTA partition"; instead it errors at the
// first Update.write() past the boundary, which we propagate.
bool OTAUpdate::applyUpdate(const String& binUrl, const String& expectedSha256,
                            size_t expectedSize) {
  if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
    Log::error(TAG, "Update.begin() failed");
    return false;
  }

  mbedtls_sha256_context sha_ctx;
  mbedtls_sha256_init(&sha_ctx);
  mbedtls_sha256_starts(&sha_ctx, 0);  // 0 = SHA-256

  size_t   written       = 0;
  uint32_t lastProgress  = 0;
  bool     writeOk       = true;

  auto cb = [&](const uint8_t* buf, size_t len) -> bool {
    if (!writeOk) return false;
    size_t w = Update.write(const_cast<uint8_t*>(buf), len);
    if (w != len) {
      Log::error(TAG, "Update.write() short write");
      writeOk = false;
      return false;
    }
    mbedtls_sha256_update(&sha_ctx, buf, len);
    written += len;
    uint32_t now = millis();
    if (now - lastProgress > 5000) {
      char msg[96];
      if (expectedSize > 0) {
        snprintf(msg, sizeof(msg), "Downloaded %u / %u bytes (%u%%)",
                 (unsigned)written, (unsigned)expectedSize,
                 (unsigned)((written * 100) / expectedSize));
      } else {
        snprintf(msg, sizeof(msg), "Downloaded %u bytes", (unsigned)written);
      }
      Log::info(TAG, msg);
      lastProgress = now;
    }
    esp_task_wdt_reset();
    yield();
    return true;
  };

  bool   transportOk = false;
  int    status      = 0;
  size_t total       = 0;

  if (WiFiManager::connected()) {
    transportOk = otaHttpGetWiFi(binUrl.c_str(), cb, &status, &total);
  }
#ifdef ENABLE_CELLULAR
  else if (Cellular::connected()) {
    if (expectedSize > 0) {
      transportOk = downloadBinaryChunkedCellular(binUrl.c_str(),
                                                  expectedSize, cb, &total);
      // The chunked path returns true only when every chunk hit 206/200,
      // so synthesize a 200 for the success-path branch below.
      if (transportOk) status = 200;
    } else {
      Log::warn(TAG, "Manifest missing 'size' - single-GET fallback (may stall on cellular)");
      transportOk = otaHttpGetCellular(binUrl.c_str(), cb, &status, &total);
    }
  }
#endif
  else {
    Log::error(TAG, "No transport for binary download");
  }

  if (!transportOk || status != 200 || !writeOk || written == 0 ||
      (expectedSize > 0 && written < expectedSize)) {
    char msg[160];
    snprintf(msg, sizeof(msg),
             "Download failed (transport=%d status=%d writeOk=%d bytes=%u/%u)",
             (int)transportOk, status, (int)writeOk,
             (unsigned)written, (unsigned)expectedSize);
    Log::error(TAG, msg);
    mbedtls_sha256_free(&sha_ctx);
    Update.abort();
    return false;
  }

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

  char msg[80];
  snprintf(msg, sizeof(msg), "Downloaded + verified %u bytes", (unsigned)written);
  Log::info(TAG, msg);
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
