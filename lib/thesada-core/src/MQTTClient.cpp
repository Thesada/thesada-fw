// thesada-fw - MQTTClient.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "MQTTClient.h"
#include <thesada_config.h>
#include "Config.h"
#include "EventBus.h"
#include "WiFiManager.h"
#include "Log.h"
#include "Shell.h"
#include "OTAUpdate.h"
#include "ota_ca_progmem.h"
#include <WiFi.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/version.h>
#include <string>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <esp_chip_info.h>

// NVS namespace + keys for per-device mTLS client certificate.
// Kept separate from Config so cert survives factory reset of config.json
// and is never exposed via /api/config or config.dump.
static const char* CERT_NS        = "thesada-tls";
static const char* CERT_KEY_CERT  = "client_cert";
static const char* CERT_KEY_KEY   = "client_key";
// CERT_MAX_LEN is now MQTTClient::CERT_MAX_LEN (header) so transports
// that load via loadClientCert can size buffers without copy-pasting
// the constant. ESP32 NVS string blob limit drives the value.

// Optional hook fired after a successful clearClientCert. Cellular
// installs one at bring-up so the modem-side cert cache invalidates
// and any live SMCONN session drops. nullptr when no transport
// registered an interest.
static std::function<void()> _onCertClearedHook = nullptr;

static const char* TAG = "MQTT";

#ifdef MQTT_TLS
WiFiClientSecure MQTTClient::_wifiClient;
// Client cert + key buffers for mTLS. Loaded from NVS on connect(), held
// live while the WiFiClientSecure session is open (the TLS stack holds
// the pointer, must outlive the handshake). Freed/NULL when no cert set.
static char*     _clientCert    = nullptr;
static char*     _clientKey     = nullptr;
static bool      _mtlsActive    = false;
static bool      _mtlsWasActive = false;  // last connect() used mTLS - need to clear WiFiClientSecure on fallback

// Persistent broker hostname buffer. PubSubClient::setServer(const char*,
// uint16_t) stores the pointer, not the contents, so we cannot hand it
// a pointer into the ArduinoJson document - Config::load() (from
// config.reload) resets the pool and invalidates the pointer, later
// reconnects then feed garbage into WiFi.hostByName() and the DNS lookup
// fails with the previous buffer bytes as the "hostname". Copy once into
// this buffer on each read.
static char      _brokerHost[96] = {0};

// Validate a PEM cert + key pair via mbedtls. Prevents feeding a bad
// buffer into WiFiClientSecure::setCertificate which would stick in the
// TLS stack and break every future reconnect until restart.
// in: cert (PEM string), key (PEM string).
// out: true if both parse as valid X.509 cert + compatible private key.
static bool validateClientCertKey(const char* cert, const char* key) {
  if (!cert || !key || !*cert || !*key) return false;

  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  int rc = mbedtls_x509_crt_parse(&crt, (const unsigned char*)cert, strlen(cert) + 1);
  mbedtls_x509_crt_free(&crt);
  if (rc != 0) return false;

  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  // mbedtls 3.x (pioarduino / IDF 5.x) added RNG callback args; 2.x has 5-arg form.
#if MBEDTLS_VERSION_MAJOR >= 3
  rc = mbedtls_pk_parse_key(&pk,
                            (const unsigned char*)key, strlen(key) + 1,
                            nullptr, 0,
                            nullptr, nullptr);
#else
  rc = mbedtls_pk_parse_key(&pk,
                            (const unsigned char*)key, strlen(key) + 1,
                            nullptr, 0);
#endif
  mbedtls_pk_free(&pk);
  return rc == 0;
}

// CA cert buffer - routed to PSRAM when BOARD_HAS_PSRAM is defined, falling
// back to internal heap otherwise. Raw char* + length instead of Arduino
// String so we can use heap_caps_malloc(MALLOC_CAP_SPIRAM) directly and
// avoid paying ~2 KB of internal SRAM for a cert that only matters during
// TLS handshake.
static char*     _caCert    = nullptr;
static size_t    _caCertLen = 0;
#else
WiFiClient       MQTTClient::_wifiClient;
#endif
PubSubClient MQTTClient::_client(_wifiClient);

MQTTMessage      MQTTClient::_queue[MQTT_QUEUE_SIZE];
uint8_t          MQTTClient::_queueHead     = 0;
uint8_t          MQTTClient::_queueTail     = 0;
uint8_t          MQTTClient::_queueCount    = 0;
uint32_t         MQTTClient::_lastAttempt   = 0;
uint32_t         MQTTClient::_retryInterval = RETRY_MIN_MS;
uint8_t          MQTTClient::_retryCount    = 0;
uint32_t         MQTTClient::_lastPublishMs   = 0;
uint32_t         MQTTClient::_minIntervalMs   = 0;
time_t           MQTTClient::_lastPublishTime = 0;
uint16_t         MQTTClient::_bufferIn       = 4096;
uint16_t         MQTTClient::_bufferOut      = 4096;

MQTTSubscription MQTTClient::_subs[MQTT_MAX_SUBS];
uint8_t          MQTTClient::_subCount = 0;
uint32_t         MQTTClient::_lastSuccessMs    = 0;
uint32_t         MQTTClient::_connectedSinceMs = 0;
bool             MQTTClient::_insecureFallback = false;
uint32_t         MQTTClient::_lastHeapPublishMs = 0;
uint32_t         MQTTClient::_lastHeapFree      = 0;
uint32_t         MQTTClient::_lowHeapSinceMs    = 0;
bool             MQTTClient::_reinitPending     = false;
bool             MQTTClient::_certApplyRebootPending = false;
uint32_t         MQTTClient::_certApplyRebootAtMs    = 0;

char             MQTTClient::_rxRing[MQTTClient::RX_RING_SIZE][96] = {};
uint32_t         MQTTClient::_rxRingTs[MQTTClient::RX_RING_SIZE]    = {};
uint8_t          MQTTClient::_rxRingHead  = 0;
uint8_t          MQTTClient::_rxRingCount = 0;

std::vector<String> MQTTClient::_retainedTopics;
bool     MQTTClient::_manifestPublished    = false;
bool     MQTTClient::_manifestDirty        = false;
uint32_t MQTTClient::_manifestDirtySinceMs = 0;
bool     MQTTClient::_retainedPublishedThisSession = false;

// Cellular fallback hint - see header. When true, publish() drops on a
// WiFi disconnect instead of enqueueing.
static bool s_fallbackPublishing = false;

// Cellular subscribe forwarder. When installed, MQTTClient::subscribe
// also calls this so the cellular MQTT session mirrors the WiFi-side
// subscription set as new entries are added at runtime.
static std::function<void(const char*)> s_subForwarder;

// Cellular publish forwarder. When installed and fallback publishing
// is active, MQTTClient::publish routes here instead of enqueueing on a
// WiFi-side disconnect. Returns true on successful hand-off to cellular,
// false if the cellular transport is not ready (publish then drops).
static std::function<bool(const char*, const char*, bool)> s_pubForwarder;

// Tracks the millis() at which fallback publishing became active, so the
// heap-stats trigger can defer its first firing until 30 s after the
// cellular handoff completes. Initial post-ACTIVE burst (battery + rssi
// + retained manifest re-emit) crowds the modem AT bus; deferring heap
// keeps that window quiet.
static uint32_t s_fallbackStartMs = 0;

void MQTTClient::setFallbackPublishing(bool active) {
  if (active && !s_fallbackPublishing) {
    // Cellular took over from WiFi. The 8-slot WiFi ring queue holds
    // stale (seconds-to-minutes old) battery/sensor samples buffered
    // during the WiFi outage. Replaying them through the cellular
    // forwarder right when battery + rssi telemetry is starting up
    // would burst the modem AT bus precisely during the window we
    // are most worried about. Drop the queue; sensors are level-
    // triggered and will refire on their next module cycle if any
    // alert condition still holds.
    if (_queueCount > 0) {
      char dmsg[80];
      snprintf(dmsg, sizeof(dmsg),
               "Cellular handoff: dropping %d queued WiFi msg(s)",
               (int)_queueCount);
      Log::info(TAG, dmsg);
      for (uint8_t i = 0; i < MQTT_QUEUE_SIZE; ++i) _queue[i].valid = false;
      _queueHead  = 0;
      _queueTail  = 0;
      _queueCount = 0;
    }
    s_fallbackStartMs = millis();
  } else if (!active) {
    s_fallbackStartMs = 0;
    // Yielding back to WiFi: clear the cellular-side republish guard so
    // the next cellular handoff republishes the retained set. WiFi
    // reconnect will run publishRetainedSet(true) via connect() and
    // re-set the flag.
    _retainedPublishedThisSession = false;
  }
  s_fallbackPublishing = active;
}

// ---------------------------------------------------------------------------

// Initialize MQTT client, load TLS cert, and set up subscriptions
void MQTTClient::begin() {
  // Bootstrap the TLS NVS namespace so readonly begin() calls from
  // hasClientCert() / loadClientCert() / getCertInfo() don't spam
  // "nvs_open failed: NOT_FOUND" to serial on first-boot devices.
  {
    Preferences prefs;
    if (prefs.begin(CERT_NS, false)) prefs.end();
  }

  JsonObject  cfg  = Config::get();
  const char* host = cfg["mqtt"]["broker"]          | "";
  uint16_t    port = cfg["mqtt"]["port"]            | 8883;
  uint32_t    ivs  = cfg["mqtt"]["send_interval_s"] | 0;
  _minIntervalMs   = ivs * 1000UL;

  if (strlen(host) == 0) {
    Log::error(TAG, "No broker in config");
    return;
  }

  _bufferIn  = cfg["mqtt"]["buffer_in"]  | 4096;
  _bufferOut = cfg["mqtt"]["buffer_out"] | 4096;
  if (_bufferIn  < 512)  _bufferIn  = 512;
  if (_bufferIn  > 8192) _bufferIn  = 8192;
  if (_bufferOut < 512)  _bufferOut = 512;
  if (_bufferOut > 8192) _bufferOut = 8192;

  // Copy broker into persistent buffer - see _brokerHost comment.
  strncpy(_brokerHost, host, sizeof(_brokerHost) - 1);
  _brokerHost[sizeof(_brokerHost) - 1] = '\0';
  _client.setServer(_brokerHost, port);
  _client.setKeepAlive(60);
  _client.setBufferSize(_bufferIn);
  _client.setCallback(onMessage);

#ifdef MQTT_TLS
  // Keep socket timeout well under the 30s hardware watchdog.
  // arduino-esp32 v2.x: setTimeout takes seconds (current platform).
  // arduino-esp32 v3.x: setTimeout takes milliseconds (future-proof).
  // Setting both ensures correct behavior across versions.
  _wifiClient.setTimeout(10);
  _wifiClient.setHandshakeTimeout(10);  // ssl handshake, always seconds

  // CA cert must be present as /ca.crt on LittleFS.
  // Allocate the cert buffer in PSRAM when available (keeps ~2 KB off the
  // internal heap, where it matters more under MQTT+Telegram fragmentation).
  // Falls back to internal heap via plain malloc on non-PSRAM targets.
  if (LittleFS.exists("/ca.crt")) {
    File cf = LittleFS.open("/ca.crt", "r");
    if (cf) {
      size_t sz = cf.size();
      if (sz > 0) {
#if defined(BOARD_HAS_PSRAM)
        _caCert = (char*)heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM);
        const char* heapTag = "PSRAM";
#else
        _caCert = (char*)malloc(sz + 1);
        const char* heapTag = "heap";
#endif
        if (_caCert) {
          size_t readBytes = cf.readBytes(_caCert, sz);
          _caCert[readBytes] = '\0';
          _caCertLen = readBytes;
          char msg[96];
          snprintf(msg, sizeof(msg), "CA cert loaded from /ca.crt (%u B in %s)",
                   (unsigned)readBytes, heapTag);
          Log::info(TAG, msg);
        } else {
          Log::error(TAG, "CA cert alloc failed");
        }
      }
      cf.close();
    }
  }
  if (!_caCert || _caCertLen == 0) {
    // Fallback to baked PROGMEM bundle. Same shared roots used by OTA -
    // a stripped LittleFS should not silently drop broker TLS validation
    // to insecure. setInsecure only fires if even the PROGMEM bundle is
    // empty (build misconfig).
    size_t pmLen = strlen_P(OTA_CA_PROGMEM);
    if (pmLen > 0) {
#if defined(BOARD_HAS_PSRAM)
      _caCert = (char*)heap_caps_malloc(pmLen + 1, MALLOC_CAP_SPIRAM);
      const char* heapTag = "PSRAM";
#else
      _caCert = (char*)malloc(pmLen + 1);
      const char* heapTag = "heap";
#endif
      if (_caCert) {
        memcpy_P(_caCert, OTA_CA_PROGMEM, pmLen);
        _caCert[pmLen] = '\0';
        _caCertLen = pmLen;
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "No /ca.crt - using baked PROGMEM CA bundle (%u B in %s)",
                 (unsigned)pmLen, heapTag);
        Log::warn(TAG, msg);
      }
    }
  }
  if (!_caCert || _caCertLen == 0) {
    Log::warn(TAG, "No /ca.crt AND PROGMEM bundle empty - TLS without cert verification");
    _wifiClient.setInsecure();
  } else {
    _wifiClient.setCACert(_caCert);
  }
#endif

  // Initialize subscription storage.
  for (int i = 0; i < MQTT_MAX_SUBS; i++) {
    _subs[i].active = false;
  }

  // Publish alert events to MQTT alert topic.
  EventBus::subscribe("alert", [](JsonObject data) {
    JsonObject  cfg    = Config::get();
    const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
    char topic[64];
    snprintf(topic, sizeof(topic), "%s/alert", prefix);
    char payload[256];
    serializeJson(data, payload, sizeof(payload));
    MQTTClient::publish(topic, payload);
  });

  // MQTT CLI: subscribe to <prefix>/cli/#, extract command from topic, payload = args.
  // Response published to <prefix>/cli/response as JSON.
  {
    JsonObject  cfg    = Config::get();
    const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
    char cliTopic[64];
    snprintf(cliTopic, sizeof(cliTopic), "%s/cli/#", prefix);

    MQTTClient::subscribe(cliTopic, [](const char* topic, const char* payload) {
      // Defer CLI command to the Shell ring - executing inside the
      // PubSubClient callback blocks keepalive and causes disconnects on
      // slow operations (LittleFS writes, config reload, etc). The
      // generic shell-line path goes through Shell::enqueue;
      // binary-payload commands (fs.write, fs.cat chunked, cert.set) and
      // the response-shape contract (one cli/response message per
      // command, JSON array of output lines) need their own handler so
      // they go through Shell::enqueueDeferred. Same ring,
      // same backpressure, single drain path.
      JsonObject  cfg    = Config::get();
      const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
      char cliPrefix[64];
      snprintf(cliPrefix, sizeof(cliPrefix), "%s/cli/", prefix);
      size_t prefixLen = strlen(cliPrefix);
      if (strncmp(topic, cliPrefix, prefixLen) != 0) return;
      const char* cmd = topic + prefixLen;
      if (strlen(cmd) == 0 || strcmp(cmd, "response") == 0) return;

      // Copy cmd + payload onto the heap so the lambda owns them past the
      // lifetime of this callback. std::string capture handles destruction
      // when the slot's DeferredFn is reset on drain.
      std::string cmdCopy(cmd);
      size_t plen = payload ? strlen(payload) : 0;
      std::string payloadCopy(payload ? payload : "", plen);

      bool ok = Shell::enqueueDeferred(
        [cmdCopy = std::move(cmdCopy), payloadCopy = std::move(payloadCopy)]() {
          MQTTClient::runCli(cmdCopy.c_str(),
                             payloadCopy.empty() ? nullptr : payloadCopy.c_str(),
                             payloadCopy.size());
        });
      if (!ok) Log::warn("MQTT", "CLI busy - command dropped");
    });
  }

  // cmd/config/set and cmd/config/push removed in v1.2.3.
  // Use cli/config.set, cli/file.write + cli/config.reload instead.

  connect();
}

// ---------------------------------------------------------------------------

// Attempt to connect to the MQTT broker with LWT
void MQTTClient::connect() {
  if (!WiFiManager::connected()) return;

  JsonObject  cfg      = Config::get();
  const char* clientId = cfg["device"]["name"]   | "thesada-node";
  const char* user     = cfg["mqtt"]["user"]      | "";
  const char* password = cfg["mqtt"]["password"]  | "";
  const char* prefix   = cfg["mqtt"]["topic_prefix"] | "thesada/node";

  // Availability topic - LWT publishes "offline" when connection drops
  char availTopic[64];
  snprintf(availTopic, sizeof(availTopic), "%s/status", prefix);

  char msg[64];
  snprintf(msg, sizeof(msg), "Connecting as %s...", clientId);
  Log::info(TAG, msg);

#ifdef MQTT_TLS
  // Force a fresh mbedtls context on every connect attempt. When a
  // prior connect failed mid-handshake, WiFiClientSecure retains the
  // underlying mbedtls_ssl_context with whatever cert/key pointers were
  // configured at that time. A subsequent setCACert / setCertificate /
  // setPrivateKey on a still-allocated context is silently ignored - the
  // next handshake reuses the stale config and the broker logs
  // "peer did not return a certificate". stop() releases the context so
  // PubSubClient's next _client.connect() call rebuilds it from scratch
  // with whatever we configure below. Cost: ~one extra TLS handshake
  // round trip on each reconnect, which we pay anyway because PubSubClient
  // tears the connection down on any failure.
  _wifiClient.stop();

  // If NTP hasn't synced yet, cert validation will fail (clock at epoch = every
  // cert looks expired). Temporarily use insecure mode and reconnect with proper
  // validation once NTP syncs in the background.
  time_t now = time(nullptr);
  if (now < 1700000000 && _caCert && _caCertLen > 0) {
    if (!_insecureFallback) {
      Log::warn(TAG, "NTP not synced - connecting without cert validation");
      _wifiClient.setInsecure();
      _insecureFallback = true;
    }
  } else if (_insecureFallback && now > 1700000000 && ESP.getMaxAllocHeap() > 40000) {
    // NTP synced since last attempt - restore cert validation
    Log::info(TAG, "NTP synced - restoring cert validation");
    _wifiClient.setCACert(_caCert);
    _insecureFallback = false;
  } else if (_insecureFallback && now > 1700000000) {
    // NTP synced but not enough contiguous heap for TLS - stay insecure
    static bool _heapWarnLogged = false;
    if (!_heapWarnLogged) {
      Log::warn(TAG, "Heap too low for TLS cert validation - staying insecure");
      _heapWarnLogged = true;
    }
  }

  // Client certificate (per-device mTLS). Loaded from NVS once and kept
  // in module-level buffers because WiFiClientSecure holds the raw char*
  // pointer for the life of the TLS session. Re-load if cert was cleared
  // externally (cert.clear via CLI) - hasClientCert() drives enable.
  //
  // Validation: parse PEM via mbedtls BEFORE setCertificate. Arduino-esp32
  // WiFiClientSecure has no clear-cert API; once a bad pointer is set, every
  // reconnect fails until restart. Catch bad PEM here and skip mTLS.
  _mtlsActive = false;
  if (!_insecureFallback && hasClientCert()) {
    if (!_clientCert) _clientCert = (char*)malloc(CERT_MAX_LEN);
    if (!_clientKey)  _clientKey  = (char*)malloc(CERT_MAX_LEN);
    if (_clientCert && _clientKey &&
        loadClientCert(_clientCert, _clientKey, CERT_MAX_LEN)) {
      if (validateClientCertKey(_clientCert, _clientKey)) {
        _wifiClient.setCertificate(_clientCert);
        _wifiClient.setPrivateKey(_clientKey);
        _mtlsActive = true;
        Log::info(TAG, "mTLS: client cert loaded from NVS");
      } else {
        Log::warn(TAG, "mTLS: stored cert/key failed mbedtls validation - password fallback");
      }
    } else {
      // Partial malloc (one buffer allocated, the other failed) or NVS
      // load failure: free whatever was allocated and null both. Holding
      // a stranded 4 KB buffer only starves the next attempt's malloc -
      // free(nullptr) is safe so this covers every failure shape.
      free(_clientCert); _clientCert = nullptr;
      free(_clientKey);  _clientKey  = nullptr;
      Log::warn(TAG, "mTLS: NVS load failed, falling back to password auth");
    }
  }
  // If previous attempt used mTLS but this one won't, clear stale pointer
  // inside WiFiClientSecure by passing nullptr. Without this, the old
  // setCertificate pointer lingers and sabotages every future connect().
  if (_mtlsWasActive && !_mtlsActive) {
    _wifiClient.setCertificate(nullptr);
    _wifiClient.setPrivateKey(nullptr);
    Log::info(TAG, "mTLS cleared - prior cert pointer reset in WiFiClientSecure");
  }
  _mtlsWasActive = _mtlsActive;
#endif

  // With mTLS active, broker uses CN as username (use_identity_as_username).
  // Skip sending user/pass - cleaner auth path, avoids mixed-mode confusion.
  bool ok;
  if (_mtlsActive) {
    ok = _client.connect(clientId, nullptr, nullptr, availTopic, 0, true, "offline");
  } else {
    ok = (strlen(user) > 0)
      ? _client.connect(clientId, user, password, availTopic, 0, true, "offline")
      : _client.connect(clientId, availTopic, 0, true, "offline");
  }

  if (ok) {
    _retryInterval = RETRY_MIN_MS;
    _retryCount    = 0;
    _lastSuccessMs    = millis();
    _connectedSinceMs = millis();
    Log::info(TAG, "Connected");

    // Enable TCP keepalive to detect dead connections faster than MQTT keepalive
    int fd = _wifiClient.fd();
    if (fd >= 0) {
      int keepAlive    = 1;
      int keepIdle     = 30;   // seconds before first probe
      int keepInterval = 10;   // seconds between probes
      int keepCount    = 3;    // failed probes before declaring dead
      setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE, &keepAlive,    sizeof(keepAlive));
      setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle,     sizeof(keepIdle));
      setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL,&keepInterval, sizeof(keepInterval));
      setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,  &keepCount,    sizeof(keepCount));
      Log::info(TAG, "TCP keepalive enabled (30s/10s/3)");
    }

    // Reset the retained-topics manifest before we start re-publishing this
    // session's retained state. publishDiscovery / publishDeviceInfo / SHT31
    // and friends will repopulate it; publishRetainedManifest() ships it.
    _retainedTopics.clear();
    _manifestPublished    = false;
    _manifestDirty        = false;
    _manifestDirtySinceMs = 0;

    resubscribeAll();
    // WiFi reconnect always republishes the retained set (force=true) so
    // a stale cellular-side flag does not block the WiFi-side refresh.
    publishRetainedSet(true);
    flushQueue();
  } else {
    char err[64];
    snprintf(err, sizeof(err), "Failed (rc=%d) - retry in %lums",
             _client.state(), _retryInterval);
    Log::warn(TAG, err);
    _retryCount++;
    _retryInterval = min(_retryInterval * 2, (uint32_t)RETRY_MAX_MS);

    // Fast restart on TLS OOM: if max contiguous block is below the ~30KB
    // mbedtls needs, retrying is pointless - only a fresh boot defragments.
    // Give 3 attempts for transient failures, then reboot.
    if (_retryCount >= 3 && ESP.getMaxAllocHeap() < 40000) {
      Log::error(TAG, "TLS alloc failed 3x with low heap - rebooting to defrag");
      delay(1000);
      ESP.restart();
    }

    // General safety net for other failure modes (broker down, auth wrong, etc.)
    if (_retryCount >= 30) {
      Log::error(TAG, "MQTT reconnect failed 30 times - rebooting");
      delay(1000);
      ESP.restart();
    }
  }
}

// ---------------------------------------------------------------------------

// Lightweight keepalive - just run PubSubClient::loop() to send/receive
// PINGREQ/PINGRESP. Safe to call during beginAll() between module inits
// so the MQTT connection survives long init sequences.
void MQTTClient::tick() {
  if (_client.connected()) {
    _client.loop();
  }
}

// ---------------------------------------------------------------------------

// Process MQTT messages and handle reconnection with backoff
void MQTTClient::loop() {
  // Preventive reboot if free heap stays below HEAP_REBOOT_FLOOR_BYTES for
  // longer than HEAP_REBOOT_HOLD_MS. Prevents the bad path where an
  // mbedtls allocation inside the TLS stack fails mid-handshake and
  // wedges the connection without a clean recovery. A reboot here lands
  // the device on a fresh, defragmented heap. Disable by defining
  // HEAP_REBOOT_FLOOR_BYTES = 0 in thesada_config.h.
  if (HEAP_REBOOT_FLOOR_BYTES > 0) {
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t now = millis();
    if (freeHeap < HEAP_REBOOT_FLOOR_BYTES) {
      if (_lowHeapSinceMs == 0) {
        _lowHeapSinceMs = now;
        char wmsg[96];
        snprintf(wmsg, sizeof(wmsg),
                 "free heap %lu B below floor %d B - watchdog armed",
                 (unsigned long)freeHeap, (int)HEAP_REBOOT_FLOOR_BYTES);
        Log::warn(TAG, wmsg);
      } else if ((now - _lowHeapSinceMs) >= (uint32_t)HEAP_REBOOT_HOLD_MS) {
        char emsg[96];
        snprintf(emsg, sizeof(emsg),
                 "free heap stuck below %d B for %d ms - rebooting",
                 (int)HEAP_REBOOT_FLOOR_BYTES, (int)HEAP_REBOOT_HOLD_MS);
        Log::error(TAG, emsg);
        delay(100);
        esp_restart();
      }
    } else if (_lowHeapSinceMs != 0) {
      Log::info(TAG, "free heap recovered - watchdog disarmed");
      _lowHeapSinceMs = 0;
    }
  }

  // CLI commands are now drained from the Shell ring -
  // see Shell::loop() drained from the main app loop. No poll needed here.

  // Periodic heap telemetry. Transport-agnostic: fires whenever any
  // transport (WiFi MQTT or cellular fallback) is publishing. Defers
  // 30 s after a fresh cellular handoff so the post-ACTIVE burst
  // (battery + rssi + retained manifest re-emit) clears first. 5 min
  // interval -> <= 288 publishes/day per metric.
  {
    uint32_t hpNow = millis();
    bool eligible = _client.connected() ||
                    (s_fallbackPublishing && s_fallbackStartMs != 0 &&
                     (hpNow - s_fallbackStartMs) >= 30000UL);
    if (eligible &&
        (_lastHeapPublishMs == 0 || (hpNow - _lastHeapPublishMs) >= HEAP_PUBLISH_MS)) {
      publishHeapStats();
    }
  }

  // Late-arriving retained topics (e.g. SHT31 discovery published after the
  // initial connect()-time manifest) re-emit after a 5 s debounce so we do
  // not thrash the broker during burst-publishes from module begin().
  if (_manifestDirty && _client.connected() &&
      (millis() - _manifestDirtySinceMs) >= 5000) {
    publishRetainedManifest();
  }

  // Deferred reboot after cert.apply. The shell handler publishes its
  // response, schedules the reboot for a few seconds later, then returns.
  // This main-loop tick performs the actual restart - only reliable way to
  // clear sticky WiFiClientSecure / mbedtls state across a cert swap on
  // classic-platform boards. Remote devices have no USB
  // fallback, so cert.apply must self-recover.
  if (_certApplyRebootPending && (int32_t)(millis() - _certApplyRebootAtMs) >= 0) {
    Log::warn(TAG, "cert.apply deferred reboot firing");
    delay(100);
    ESP.restart();
  }

  // Deferred reconnect after reinitSubscriptions() - runs on a clean stack
  // so TLS handshake has enough room (~10KB for mbedtls).
  if (_reinitPending) {
    _reinitPending = false;
    JsonObject  cfgR = Config::get();
    const char* host = cfgR["mqtt"]["broker"] | "";
    uint16_t    port = cfgR["mqtt"]["port"]   | 8883;
    // Refresh the persistent buffer so the re-parsed pool cannot dangle
    // the pointer PubSubClient holds across future reconnects. See
    // _brokerHost comment at the top of the file.
    strncpy(_brokerHost, host, sizeof(_brokerHost) - 1);
    _brokerHost[sizeof(_brokerHost) - 1] = '\0';
    _client.setServer(_brokerHost, port);
    _retryInterval = RETRY_MIN_MS;
    connect();
  }

  if (_client.connected()) {
    // NTP upgrade: if we connected insecure and NTP has since synced,
    // disconnect and reconnect with proper cert validation.
    // Skip if largest contiguous block is below 40KB (TLS handshake needs ~30KB).
    if (_insecureFallback && time(nullptr) > 1700000000 && ESP.getMaxAllocHeap() > 40000) {
      Log::info(TAG, "NTP synced - upgrading to cert-validated connection");
      _connectedSinceMs = 0;  // suppress "lost after 0 seconds" log
      _client.disconnect();
      _wifiClient.stop();
      _retryInterval = RETRY_MIN_MS;
      return;
    }

    // Watchdog: force reconnect if no successful activity in WATCHDOG_MS
    uint32_t now = millis();
    if (_lastSuccessMs > 0 && (now - _lastSuccessMs > WATCHDOG_MS)) {
      uint32_t uptime = (now - _connectedSinceMs) / 1000;
      char wmsg[80];
      snprintf(wmsg, sizeof(wmsg), "Watchdog: no activity for %lus - forcing reconnect (up %lus)", WATCHDOG_MS / 1000, uptime);
      Log::warn(TAG, wmsg);
      _client.disconnect();
      _wifiClient.stop();
      _lastSuccessMs = 0;
      _retryInterval = RETRY_MIN_MS;
      return;
    }

    _client.loop();
    _lastSuccessMs = millis();  // MQTT loop succeeded (keepalive worked)

    // Drain queued messages, respecting minimum send interval.
    if (_queueCount > 0) {
      if (_minIntervalMs == 0 || now - _lastPublishMs >= _minIntervalMs) {
        MQTTMessage& msg = _queue[_queueHead];
        if (msg.valid) {
          if (_client.publish(msg.topic, msg.payload)) {
            _lastPublishMs   = now;
            _lastPublishTime = time(nullptr);
            _lastSuccessMs   = now;
            msg.valid = false;
            _queueHead = (_queueHead + 1) % MQTT_QUEUE_SIZE;
            _queueCount--;
          }
        }
      }
    }
    return;
  }

  // Connection lost - log uptime
  if (_connectedSinceMs > 0) {
    uint32_t uptime = (millis() - _connectedSinceMs) / 1000;
    char dmsg[64];
    snprintf(dmsg, sizeof(dmsg), "Connection lost after %lu seconds", uptime);
    Log::warn(TAG, dmsg);
    _connectedSinceMs = 0;
  }

  if (!WiFiManager::connected()) return;

  uint32_t now = millis();
  if (now - _lastAttempt >= _retryInterval) {
    _lastAttempt = now;
    connect();
  }
}

// ---------------------------------------------------------------------------

// Publish a message, queuing it if disconnected or rate-limited
void MQTTClient::publish(const char* topic, const char* payload) {
  if (_client.connected()) {
    if (_minIntervalMs > 0) {
      uint32_t now = millis();
      if (now - _lastPublishMs < _minIntervalMs) {
        enqueue(topic, payload);
        return;
      }
      _lastPublishMs   = now;
      _lastPublishTime = time(nullptr);
    }
    _client.publish(topic, payload);
    _lastPublishTime = time(nullptr);
    return;
  }
  // WiFi MQTT is down. If cellular has taken over, route through the
  // installed publish forwarder so the broker still gets the message
  // via the modem-native MQTT session. The WiFi ring is reserved for
  // WiFi - we do NOT enqueue under fallback or it fills 8/8 forever
  // and every new publish triggers "Queue full - dropping oldest"
  // eviction.
  if (s_fallbackPublishing) {
    if (s_pubForwarder) s_pubForwarder(topic, payload, false);
    return;
  }
  enqueue(topic, payload);
}

// Publish a retained message (for HA discovery configs).
// Records the topic in the retained-topics manifest so the platform can
// clear it on device delete. Routes through the cellular forwarder when
// WiFi MQTT is down and cellular fallback is publishing, so HA discovery
// configs / availability / device info still land on the broker.
void MQTTClient::publishRetained(const char* topic, const char* payload) {
  if (_client.connected()) {
    _client.publish(topic, (const uint8_t*)payload, strlen(payload), true);
    recordRetainedTopic(topic);
    return;
  }
  if (s_fallbackPublishing && s_pubForwarder) {
    if (s_pubForwarder(topic, payload, true)) {
      recordRetainedTopic(topic);
    }
  }
}

// Append a topic to the retained-topics manifest, deduping case-sensitively.
// Caller is responsible for invoking this only when a retained publish was
// actually issued. Manifest is reset at the start of each connect() so it
// always reflects the current session's footprint.
// in: topic. out: appended to _retainedTopics if not already present.
void MQTTClient::recordRetainedTopic(const char* topic) {
  if (!topic || !*topic) return;
  for (const auto& t : _retainedTopics) {
    if (t == topic) return;
  }
  _retainedTopics.emplace_back(topic);
  if (_manifestPublished) {
    _manifestDirty = true;
    _manifestDirtySinceMs = millis();
  }
}

// Publish a retained JSON array of every retained topic this device owns to
// <prefix>/info/retained_topics. Called after publishDiscovery + module
// discovery have run so the list is complete. Lets a controller enumerate
// + clean up retained state when a device is unprovisioned.
// in: none (reads _retainedTopics + topic_prefix from Config).
// out: publishes one retained MQTT message; topic itself is added to the
// manifest list pre-publish so the device can also clean its own info.
void MQTTClient::publishRetainedManifest() {
  if (!_client.connected() && !s_fallbackPublishing) return;

  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";

  char topic[96];
  snprintf(topic, sizeof(topic), "%s/info/retained_topics", prefix);

  // Include the manifest topic itself so a delete sweep also clears it.
  recordRetainedTopic(topic);

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto& t : _retainedTopics) arr.add(t);

  // Conservative budget: ~30-50 short topics * ~80 B = ~4 KB. Keep on heap.
  String payload;
  serializeJson(doc, payload);

  bool ok = false;
  if (_client.connected()) {
    ok = _client.publish(topic, (const uint8_t*)payload.c_str(), payload.length(), true);
  } else if (s_pubForwarder) {
    ok = s_pubForwarder(topic, payload.c_str(), true);
  }
  if (!ok) return;
  _manifestPublished    = true;
  _manifestDirty        = false;
  _manifestDirtySinceMs = 0;
  Log::info(TAG, (String("retained-topics manifest: ") + _retainedTopics.size() + " entries").c_str());
}

// ---------------------------------------------------------------------------

// Republish the device's full retained-state set: availability "online", HA
// discovery configs, /info, retained-topics manifest. Routes through
// publishRetained so the cellular forwarder picks it up when WiFi MQTT is
// down.
//
// Called from connect() (WiFi reconnect, force=true) and from
// CellularModule on the STANDBY -> ACTIVE transition (force=false).
// The session flag prevents republishing on every cellular STANDBY -> ACTIVE
// bounce within a single fallback window; setFallbackPublishing(false)
// clears it so a fresh fallback window republishes.
//
// in:  force - skip the session-flag check.
// out: none.
void MQTTClient::publishRetainedSet(bool force) {
  if (!_client.connected() && !s_fallbackPublishing) return;
  if (!force && _retainedPublishedThisSession) return;

  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";

  char availTopic[64];
  snprintf(availTopic, sizeof(availTopic), "%s/status", prefix);
  publishRetained(availTopic, "online");

  publishDiscovery();
  publishDeviceInfo();
  publishRetainedManifest();

  _retainedPublishedThisSession = true;
}

// ---------------------------------------------------------------------------

// Register a topic subscription with a callback
void MQTTClient::subscribe(const char* topic, MQTTCallback callback) {
  if (_subCount >= MQTT_MAX_SUBS) {
    Log::error(TAG, "Max subscriptions reached");
    return;
  }

  MQTTSubscription& sub = _subs[_subCount];
  strncpy(sub.topic, topic, sizeof(sub.topic) - 1);
  sub.topic[sizeof(sub.topic) - 1] = '\0';
  sub.callback = callback;
  sub.active = true;
  _subCount++;

  char msg[128];
  snprintf(msg, sizeof(msg), "Registered sub: %s (%d/%d)", topic, _subCount, MQTT_MAX_SUBS);
  Log::info(TAG, msg);

  // If already connected, subscribe immediately and feed keepalive.
  // Bulk subscription during Lua init can take >10s total - without
  // tick() the broker or HAProxy drops the idle TCP connection.
  if (_client.connected()) {
    _client.subscribe(topic);
    _client.loop();
  }

  // If cellular has installed a forwarder, mirror this subscription onto
  // the cellular MQTT session too.
  if (s_subForwarder) s_subForwarder(topic);
}

// ---------------------------------------------------------------------------

// Capture topic in the rxRing and fire every active subscription whose
// topic matches by exact string OR trailing /# OR trailing /+. Shared
// between WiFi (onMessage) and cellular (dispatchInbound) so the two
// transports never drift on wildcard semantics.
void MQTTClient::matchAndDispatch(const char* topic, const char* payload) {
  strncpy(_rxRing[_rxRingHead], topic, sizeof(_rxRing[0]) - 1);
  _rxRing[_rxRingHead][sizeof(_rxRing[0]) - 1] = '\0';
  _rxRingTs[_rxRingHead] = millis();
  _rxRingHead = (_rxRingHead + 1) % RX_RING_SIZE;
  if (_rxRingCount < RX_RING_SIZE) _rxRingCount++;

  for (uint8_t i = 0; i < _subCount; i++) {
    if (!_subs[i].active) continue;
    size_t slen = strlen(_subs[i].topic);
    // Trailing /# = multi-level wildcard.
    if (slen >= 2 && _subs[i].topic[slen - 1] == '#' && _subs[i].topic[slen - 2] == '/') {
      if (strncmp(_subs[i].topic, topic, slen - 1) == 0) {
        _subs[i].callback(topic, payload);
      }
    }
    // Trailing /+ = single-level wildcard. Match if the prefix up to /+
    // matches and the remainder of the topic has no further '/'.
    else if (slen >= 2 && _subs[i].topic[slen - 1] == '+' && _subs[i].topic[slen - 2] == '/') {
      if (strncmp(_subs[i].topic, topic, slen - 1) == 0) {
        const char* rest = topic + (slen - 1);
        if (*rest != '\0' && strchr(rest, '/') == nullptr) {
          _subs[i].callback(topic, payload);
        }
      }
    }
    else if (strcmp(_subs[i].topic, topic) == 0) {
      _subs[i].callback(topic, payload);
    }
  }
}

// Route incoming messages to matching subscription callbacks.
// Public dispatcher used by Cellular::pumpInbound to route +SMSUB: URCs
// through the same subscription callbacks as the WiFi onMessage path.
void MQTTClient::dispatchInbound(const char* topic, const char* payload, size_t length) {
  matchAndDispatch(topic, payload);
  (void)length;  // payload is null-terminated by caller
}

// Iterate every active subscription topic. Cellular bring-up uses this
// to issue AT+SMSUB on every entry so the cellular transport carries the
// same subscription set as the WiFi transport.
void MQTTClient::forEachSubscription(std::function<void(const char*)> fn) {
  for (uint8_t i = 0; i < _subCount; i++) {
    if (_subs[i].active) fn(_subs[i].topic);
  }
}

void MQTTClient::setSubscribeForwarder(std::function<void(const char*)> fn) {
  s_subForwarder = fn;
}

void MQTTClient::setPublishForwarder(std::function<bool(const char*, const char*, bool)> fn) {
  s_pubForwarder = fn;
}

void MQTTClient::onMessage(char* topic, uint8_t* payload, unsigned int length) {
  // Null-terminate the payload - heap-allocated to avoid stack overflow on WROOM-32.
  size_t bufSize = length + 1;
  char* payloadBuf = (char*)malloc(bufSize);
  if (!payloadBuf) return;
  memcpy(payloadBuf, payload, length);
  payloadBuf[length] = '\0';

  matchAndDispatch(topic, payloadBuf);
  free(payloadBuf);
}

// ---------------------------------------------------------------------------

// Run a CLI command on the main loop (Shell::enqueueDeferred drain).
// The PubSubClient callback already extracted cmd from the topic and
// copied payload onto the heap (owned by the std::function capture);
// this just dispatches by command name + binary-protocol special case.
void MQTTClient::runCli(const char* cmd, const char* payload, size_t plen) {
  if (!cmd || strlen(cmd) == 0) return;

  // Caller already filtered out empty cmd + the response topic itself,
  // but the special-case branches still expect the locals to exist for
  // the response-publish path so resolve prefix here.
  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";

  // CLI correlation envelope: `{"req_id":<id>, "args":<string>}` where both
  // fields are optional. req_id is echoed back on every cli/response so the
  // app can match requests against responses on the shared cli/response
  // topic. args, when present as a string, is unwrapped and used as the
  // command payload for every downstream handler (binary fs.write /
  // fs.append / cert.set + chunked fs.cat + Shell::execute fall-through).
  // Non-envelope callers (raw plain-text args, raw binary path\ncontent)
  // keep working - the envelope is recognised by payload[0]=='{' + valid
  // JSON object shape; anything else falls through untouched.
  //
  // reqDoc stays in scope for the whole function so reqId and the args
  // string view remain valid at every publish + dispatch site.
  JsonDocument reqDoc;
  JsonVariantConst reqId;
  bool hasReqId = false;
  bool reqIdOnlyPayload = false;
  if (payload && plen > 0 && payload[0] == '{') {
    if (deserializeJson(reqDoc, payload, plen) == DeserializationError::Ok &&
        reqDoc.is<JsonObject>()) {
      JsonObjectConst obj = reqDoc.as<JsonObjectConst>();
      if (!obj["req_id"].isNull()) {
        reqId = obj["req_id"];
        hasReqId = true;
      }
      // Unwrap args: every downstream handler sees the inner string
      // instead of the JSON envelope. Binary handlers still get raw bytes
      // (the path\ncontent / type\nPEM format lives inside the args
      // string). fs.cat chunked still gets "path offset length".
      const char* argsStr = obj["args"];
      if (argsStr) {
        payload = argsStr;
        plen    = strlen(argsStr);
      }
      // {"req_id":...} with no other keys (or {"req_id":..., "args":""}
      // after unwrap) = no-arg command. Falls through to the Shell path
      // bare instead of carrying the envelope as a literal arg.
      reqIdOnlyPayload = hasReqId && (obj.size() == 1 ||
                                      (argsStr && plen == 0));
    }
  }

  {

    // Special case: fs.write / fs.append / file.write (legacy alias)
    // First line of payload is the path, rest is content.
    // fs.write truncates (mode "w"), fs.append appends (mode "a").
    if ((strcmp(cmd, "fs.write") == 0 || strcmp(cmd, "file.write") == 0 ||
         strcmp(cmd, "fs.append") == 0) && payload && plen > 0) {
      const char* mode = (strcmp(cmd, "fs.append") == 0) ? "a" : "w";
      const char* nl = strchr(payload, '\n');
      JsonDocument resp;
      resp["cmd"] = cmd;
      if (hasReqId) resp["req_id"] = reqId;
      if (!nl) {
        resp["ok"] = false;
        resp["output"][0] = "Usage: payload = <path>\\n<content>";
      } else {
        char path[64];
        size_t pathLen = min((size_t)(nl - payload), sizeof(path) - 1);
        memcpy(path, payload, pathLen);
        path[pathLen] = '\0';
        const char* content = nl + 1;
        size_t contentLen = plen - pathLen - 1;

        // Reject path traversal before any LittleFS call - the cli/<cmd>
        // topic is the device's remote attack surface (broker ACL is the
        // only thing between an external publisher and this handler).
        if (!Shell::pathSafe(path)) {
          resp["ok"] = false;
          resp["output"][0] = "Invalid path";
          char respTopic[64];
          snprintf(respTopic, sizeof(respTopic), "%s/cli/response", prefix);
          size_t bufSz = _bufferOut > 0 ? _bufferOut : 4096;
          char* rp = (char*)malloc(bufSz);
          if (rp) { serializeJson(resp, rp, bufSz); MQTTClient::publish(respTopic, rp); free(rp); }
          goto cleanup;
        }

        fs::FS* fs = Shell::resolveFS(path);
        const char* fsPath = Shell::stripPrefix(path);
        File f = fs->open(fsPath, mode);
        if (f) {
          size_t written = f.write((const uint8_t*)content, contentLen);
          f.close();
          resp["ok"] = true;
          char msg[64];
          snprintf(msg, sizeof(msg), "%s %d bytes to %s",
                   mode[0] == 'a' ? "Appended" : "Wrote", (int)written, path);
          resp["output"][0] = msg;
          Log::info("MQTT", msg);
        } else {
          resp["ok"] = false;
          resp["output"][0] = "Failed to open file";
        }
      }
      char respTopic[64];
      snprintf(respTopic, sizeof(respTopic), "%s/cli/response", prefix);
      size_t bufSz = _bufferOut > 0 ? _bufferOut : 4096;
      char* rp = (char*)malloc(bufSz);
      if (rp) { serializeJson(resp, rp, bufSz); MQTTClient::publish(respTopic, rp); free(rp); }
      goto cleanup;
    }

    // Special case: fs.cat with offset/length for chunked reads.
    // Payload: "<path> <offset> <length>" - reads a byte range and returns
    // JSON with offset, length, total, done, and data (JSON-escaped text).
    // Without offset/length, falls through to Shell::execute (line-by-line).
    if (strcmp(cmd, "fs.cat") == 0 && payload && plen > 0) {
      // Parse: path offset length (space-separated)
      char pbuf[256];
      strncpy(pbuf, payload, min(plen, sizeof(pbuf) - 1));
      pbuf[min(plen, sizeof(pbuf) - 1)] = '\0';

      char* path = strtok(pbuf, " ");
      char* offStr = strtok(nullptr, " ");
      char* lenStr = strtok(nullptr, " ");

      // Only handle chunked mode (with offset + length)
      if (path && offStr && lenStr) {
        size_t offset = (size_t)atol(offStr);
        size_t chunkLen = (size_t)atol(lenStr);
        // Cap chunk to half of output buffer (response JSON has overhead)
        size_t maxChunk = _bufferOut > 512 ? _bufferOut / 2 : 512;
        if (chunkLen == 0) chunkLen = maxChunk;
        if (chunkLen > maxChunk) chunkLen = maxChunk;

        JsonDocument resp;
        resp["cmd"] = cmd;
        if (hasReqId) resp["req_id"] = reqId;

        // Reject path traversal before LittleFS - same surface as fs.write
        // above, same broker-ACL-only mitigation pre-pathSafe.
        if (!Shell::pathSafe(path)) {
          resp["ok"] = false;
          resp["output"][0] = "Invalid path";
          char respTopic[64];
          snprintf(respTopic, sizeof(respTopic), "%s/cli/response", prefix);
          size_t bufSz = _bufferOut > 0 ? _bufferOut : 4096;
          char* rp = (char*)malloc(bufSz);
          if (rp) { serializeJson(resp, rp, bufSz); MQTTClient::publish(respTopic, rp); free(rp); }
          goto cleanup;
        }

        fs::FS* catfs = Shell::resolveFS(path);
        const char* catFsPath = Shell::stripPrefix(path);
        File f = catfs->open(catFsPath, "r");
        if (!f) {
          resp["ok"] = false;
          resp["output"][0] = "File not found";
        } else {
          size_t total = f.size();
          resp["ok"] = true;
          resp["total"] = (int)total;
          resp["offset"] = (int)offset;

          if (offset >= total) {
            resp["length"] = 0;
            resp["done"] = true;
            resp["data"] = "";
          } else {
            f.seek(offset);
            size_t avail = total - offset;
            size_t toRead = min(chunkLen, avail);
            char* buf = (char*)malloc(toRead + 1);
            if (buf) {
              size_t got = f.read((uint8_t*)buf, toRead);
              buf[got] = '\0';
              resp["length"] = (int)got;
              resp["done"] = (offset + got >= total);
              // ArduinoJson escapes the string for JSON automatically
              resp["data"] = buf;
              free(buf);
            } else {
              resp["ok"] = false;
              resp["output"][0] = "malloc failed";
            }
          }
          f.close();
        }

        char respTopic[64];
        snprintf(respTopic, sizeof(respTopic), "%s/cli/response", prefix);
        size_t bufSz = _bufferOut > 0 ? _bufferOut : 4096;
        char* rp = (char*)malloc(bufSz);
        if (rp) { serializeJson(resp, rp, bufSz); MQTTClient::publish(respTopic, rp); free(rp); }
        goto cleanup;
      }
      // No offset/length - fall through to Shell::execute for line-by-line
    }

    // Special case: cert.set - per-device mTLS client cert/key push.
    // Payload: "<type>\n<PEM content>" where type is "client_cert" or
    // "client_key". Writes to NVS (survives factory reset of config.json).
    // cert.apply (non-binary command) disconnects MQTT and reconnects with
    // mTLS once both halves are present.
    if (strcmp(cmd, "cert.set") == 0 && payload && plen > 0) {
      JsonDocument resp;
      resp["cmd"] = cmd;
      if (hasReqId) resp["req_id"] = reqId;

      const char* nl = strchr(payload, '\n');
      if (!nl) {
        resp["ok"] = false;
        resp["output"][0] = "Usage: payload = <type>\\n<PEM>  (type: client_cert|client_key)";
      } else {
        char type[32];
        size_t typeLen = min((size_t)(nl - payload), sizeof(type) - 1);
        memcpy(type, payload, typeLen);
        type[typeLen] = '\0';
        const char* pem = nl + 1;
        size_t pemLen = plen - typeLen - 1;

        if (pemLen >= CERT_MAX_LEN) {
          resp["ok"] = false;
          resp["output"][0] = "PEM too large (>4000 B)";
        } else if (strcmp(type, "client_cert") == 0) {
          // Null-terminate a copy; NVS putString needs a C string.
          char* buf = (char*)malloc(pemLen + 1);
          if (!buf) {
            resp["ok"] = false;
            resp["output"][0] = "malloc failed";
          } else {
            memcpy(buf, pem, pemLen);
            buf[pemLen] = '\0';
            bool ok = storeClientCert(buf, nullptr);
            free(buf);
            resp["ok"] = ok;
            resp["output"][0] = ok ? "Client cert stored in NVS" : "NVS write failed";
          }
        } else if (strcmp(type, "client_key") == 0) {
          char* buf = (char*)malloc(pemLen + 1);
          if (!buf) {
            resp["ok"] = false;
            resp["output"][0] = "malloc failed";
          } else {
            memcpy(buf, pem, pemLen);
            buf[pemLen] = '\0';
            bool ok = storeClientCert(nullptr, buf);
            // Zero the heap copy of the private key before free - leaving
            // it lingering in heap means any future feature that dumps /
            // inspects heap (debug command, crash uploader, remote heap
            // inspector) leaks the key. mbedtls_platform_zeroize is not
            // optimised away by the compiler.
            mbedtls_platform_zeroize(buf, pemLen + 1);
            free(buf);
            resp["ok"] = ok;
            resp["output"][0] = ok ? "Client key stored in NVS" : "NVS write failed";
          }
        } else {
          resp["ok"] = false;
          resp["output"][0] = "Unknown type - expected client_cert or client_key";
        }
      }

      char respTopic[64];
      snprintf(respTopic, sizeof(respTopic), "%s/cli/response", prefix);
      size_t bufSz = _bufferOut > 0 ? _bufferOut : 4096;
      char* rp = (char*)malloc(bufSz);
      if (rp) { serializeJson(resp, rp, bufSz); MQTTClient::publish(respTopic, rp); free(rp); }
      goto cleanup;
    }

    // Build command line: "<cmd> <payload>". An empty JSON object payload
    // is treated as no-arg - the platform side substitutes "{}" for empty
    // payloads as a SIM7080G workaround (empty SMSUB URCs get silently
    // dropped on cellular), and Shell command handlers should not see
    // that substitution as a literal argument.
    //
    // Contract for new MQTT CLI commands: a command reached here goes
    // through Shell::execute as a flat "<cmd> <payload>" string. It must
    // therefore either (a) take its payload as plain space-delimited
    // args, or (b) accept "{}" / a req_id-only envelope as a valid
    // no-arg invocation. A command that needs a STRUCTURED JSON payload
    // cannot use this path - "{}" would be swallowed as no-arg and any
    // other JSON passed verbatim as a single garbage arg. Such commands
    // must be added as a special-case binary-payload handler ABOVE this
    // point (see cert.set / the offset/length file-read block).
    char line[1024];
    bool isEmptyJson =
      payload && plen == 2 && payload[0] == '{' && payload[1] == '}';
    // {"req_id": ...} with no other keys is also a no-arg invocation -
    // the envelope is purely for correlation, not arguments. Without
    // this the envelope JSON would be passed verbatim as a literal arg
    // and Shell command parsers would see it as garbage.
    if (payload && plen > 0 && !isEmptyJson && !reqIdOnlyPayload) {
      snprintf(line, sizeof(line), "%s %s", cmd, payload);
    } else {
      snprintf(line, sizeof(line), "%s", cmd);
    }

    // Execute through Shell and collect output, paginating the response so
    // a command that produces more output than fits in one cli/response
    // payload (fs.ls on a large SD dir, help, module dumps) is not silently
    // truncated by serializeJson. The output sink measures the running JSON
    // size; when the next line would overflow the publish buffer it ships
    // the current page with more=true and starts a fresh one. The final
    // page carries more=false. Single-page output - the common case - is
    // page 0 / more=false, the same shape any consumer that ignores the
    // new fields already sees.
    size_t bufSz = _bufferOut > 0 ? _bufferOut : 4096;
    // Headroom for the envelope serializeJson writes around the output
    // array (cmd, req_id, ok, page, more keys) plus the trailing NUL.
    const size_t PAGE_MARGIN = 160;
    size_t pageLimit = bufSz > PAGE_MARGIN ? bufSz - PAGE_MARGIN : bufSz / 2;

    JsonDocument resp;
    int pageNum = 0;

    auto startPage = [&]() -> JsonArray {
      resp.clear();
      resp["cmd"] = cmd;
      if (hasReqId) resp["req_id"] = reqId;
      resp["ok"]   = true;
      resp["page"] = pageNum;
      return resp["output"].to<JsonArray>();
    };

    auto publishPage = [&](bool more) {
      resp["more"] = more;
      // Re-read prefix every publish - config.reload mid-command may have
      // invalidated it, and a multi-page command spans more wall time.
      JsonObject cfgAfter = Config::get();
      const char* pfxAfter = cfgAfter["mqtt"]["topic_prefix"] | "thesada/node";
      char respTopic[64];
      snprintf(respTopic, sizeof(respTopic), "%s/cli/response", pfxAfter);
      char* rp = (char*)malloc(bufSz);
      if (rp) {
        serializeJson(resp, rp, bufSz);
        MQTTClient::publish(respTopic, rp);
        free(rp);
      }
    };

    JsonArray output = startPage();

    Shell::execute(line, [&](const char* outLine) {
      // strlen + slack covers ASCII shell output (the common path); a
      // pathological line full of escaped bytes still publishes, it just
      // risks one truncated page rather than dropping silently.
      size_t lineCost = strlen(outLine) + 8;
      if (output.size() > 0 && measureJson(resp) + lineCost > pageLimit) {
        publishPage(true);
        pageNum++;
        output = startPage();
      }
      output.add(outLine);
    });

    publishPage(false);
  }

cleanup:
  // Payload is owned by the std::function capture in the Shell ring slot;
  // it gets released when the slot is reset on drain. Nothing to free here.
  return;
}

// ---------------------------------------------------------------------------

// Clear subscriptions and re-register core MQTT topics (cli/#, cmd/ota)
// with the current config prefix. Called after config.reload when network
// keys changed. Does NOT re-register EventBus handlers or reload TLS certs.
void MQTTClient::reinitSubscriptions() {
  for (int i = 0; i < MQTT_MAX_SUBS; i++) _subs[i].active = false;
  _subCount = 0;

  JsonObject  cfg    = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  char cliTopic[64];
  snprintf(cliTopic, sizeof(cliTopic), "%s/cli/#", prefix);

  // Same Shell::enqueueDeferred path as begin()'s subscribe so config.reload
  // does not split CLI dispatch between two mechanisms.
  MQTTClient::subscribe(cliTopic, [](const char* topic, const char* payload) {
    JsonObject  cfgInner    = Config::get();
    const char* prefixInner = cfgInner["mqtt"]["topic_prefix"] | "thesada/node";
    char cliPrefixInner[64];
    snprintf(cliPrefixInner, sizeof(cliPrefixInner), "%s/cli/", prefixInner);
    size_t prefixLen = strlen(cliPrefixInner);
    if (strncmp(topic, cliPrefixInner, prefixLen) != 0) return;
    const char* cmd = topic + prefixLen;
    if (strlen(cmd) == 0 || strcmp(cmd, "response") == 0) return;

    std::string cmdCopy(cmd);
    size_t plen = payload ? strlen(payload) : 0;
    std::string payloadCopy(payload ? payload : "", plen);

    bool ok = Shell::enqueueDeferred(
      [cmdCopy = std::move(cmdCopy), payloadCopy = std::move(payloadCopy)]() {
        MQTTClient::runCli(cmdCopy.c_str(),
                           payloadCopy.empty() ? nullptr : payloadCopy.c_str(),
                           payloadCopy.size());
      });
    if (!ok) Log::warn("MQTT", "CLI busy - command dropped");
  });

  OTAUpdate::begin();

  if (_client.connected()) {
    _client.disconnect();
    Log::info(TAG, "Disconnected for subscription reinit");
  }
  _reinitPending = true;
}

// Re-subscribe all active topics after reconnect
void MQTTClient::resubscribeAll() {
  for (uint8_t i = 0; i < _subCount; i++) {
    if (!_subs[i].active) continue;
    _client.subscribe(_subs[i].topic);
    char msg[128];
    snprintf(msg, sizeof(msg), "Subscribed: %s", _subs[i].topic);
    Log::info(TAG, msg);
  }
}

// ---------------------------------------------------------------------------

// Publish Home Assistant MQTT discovery configs for all sensors
void MQTTClient::publishDiscovery() {
  JsonObject cfg = Config::get();
  bool enabled = cfg["mqtt"]["ha_discovery"] | true;
  if (!enabled) {
    Log::info(TAG, "HA discovery disabled");
    return;
  }

  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  const char* devName = cfg["device"]["friendly_name"] | cfg["device"]["name"] | "Thesada Node";
  const char* devId = cfg["device"]["name"] | "thesada_node";

  // Availability topic
  char availTopic[64];
  snprintf(availTopic, sizeof(availTopic), "%s/status", prefix);

  // Helper: build slug from name (lowercase, underscores)
  auto makeSlug = [](const char* name, char* slug, size_t sz) {
    strncpy(slug, name, sz - 1);
    slug[sz - 1] = '\0';
    for (char* p = slug; *p; p++) { if (*p == ' ') *p = '_'; *p = tolower(*p); }
  };

  // Helper: publish a single discovery config (retained).
  // Uses per-sensor state topics with simple {{value}} template.
  auto disc = [&](const char* component, const char* uid, const char* name,
                  const char* stateTopic,
                  const char* unit, const char* devClass, const char* stateClass,
                  const char* entityCategory = nullptr) {
    JsonDocument doc;
    doc["name"] = name;
    doc["stat_t"] = stateTopic;
    doc["uniq_id"] = uid;
    doc["avty_t"] = availTopic;
    if (unit && strlen(unit) > 0)        doc["unit_of_meas"] = unit;
    if (devClass && strlen(devClass) > 0) doc["dev_cla"] = devClass;
    if (stateClass && strlen(stateClass) > 0) doc["stat_cla"] = stateClass;
    if (entityCategory && strlen(entityCategory) > 0) doc["ent_cat"] = entityCategory;

    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"] = devId;
    dev["name"] = devName;
    dev["mf"] = "Thesada";
    dev["mdl"] = "Base Node";
    dev["sw"] = FIRMWARE_VERSION;

    char topic[128];
    snprintf(topic, sizeof(topic), "homeassistant/%s/%s/%s/config", component, devId, uid);

    char payload[640];
    serializeJson(doc, payload, sizeof(payload));
    publishRetained(topic, payload);
    // Drain TCP buffer between retained publishes. Without this, small
    // MQTT buffers (CYD 1024B) overflow and spam EAGAIN socket errors.
    // Cellular forwarder serializes via ATGuard so the loop()/yield is a
    // no-op on that path but harmless.
    if (_client.connected()) _client.loop();
    yield();
  };

  char slug[32], uid[48], stBuf[96];

  // -- Temperature sensors --
  JsonArray sensors = cfg["temperature"]["sensors"].as<JsonArray>();
  if (sensors) {
    const char* tunit = cfg["temperature"]["unit"] | "C";
    const char* haUnit = (tunit[0] == 'F' || tunit[0] == 'f') ? "\xC2\xB0""F" : "\xC2\xB0""C";
    for (JsonObject s : sensors) {
      const char* sname = s["name"] | "unknown";
      makeSlug(sname, slug, sizeof(slug));
      snprintf(uid, sizeof(uid), "%s_%s_temp", devId, slug);
      snprintf(stBuf, sizeof(stBuf), "%s/sensor/temperature/%s", prefix, slug);
      disc("sensor", uid, sname, stBuf, haUnit, "temperature", "measurement");
    }
  }

  // -- ADS1115 channels --
  JsonArray channels = cfg["ads1115"]["channels"].as<JsonArray>();
  if (channels) {
    for (JsonObject ch : channels) {
      const char* cname = ch["name"] | "unknown";
      makeSlug(cname, slug, sizeof(slug));

      // Current (A)
      snprintf(uid, sizeof(uid), "%s_%s_current", devId, slug);
      snprintf(stBuf, sizeof(stBuf), "%s/sensor/current/%s", prefix, slug);
      disc("sensor", uid, cname, stBuf, "A", "current", "measurement");

      // Power (W)
      char pname[64];
      snprintf(pname, sizeof(pname), "%s Power", cname);
      snprintf(uid, sizeof(uid), "%s_%s_power", devId, slug);
      snprintf(stBuf, sizeof(stBuf), "%s/sensor/power/%s", prefix, slug);
      disc("sensor", uid, pname, stBuf, "W", "power", "measurement");
    }
  }

  // -- Battery --
  bool battEnabled = cfg["battery"]["enabled"] | true;
  if (battEnabled) {
    snprintf(uid, sizeof(uid), "%s_battery_percent", devId);
    snprintf(stBuf, sizeof(stBuf), "%s/sensor/battery/percent", prefix);
    disc("sensor", uid, "Battery", stBuf, "%", "battery", "measurement");

    snprintf(uid, sizeof(uid), "%s_battery_voltage", devId);
    snprintf(stBuf, sizeof(stBuf), "%s/sensor/battery/voltage", prefix);
    disc("sensor", uid, "Battery Voltage", stBuf, "V", "voltage", "measurement");

    snprintf(uid, sizeof(uid), "%s_battery_charging", devId);
    snprintf(stBuf, sizeof(stBuf), "%s/sensor/battery/charging", prefix);
    disc("sensor", uid, "Battery Charge State", stBuf, "", "", "");
  }

  // -- WiFi diagnostics (disabled by default in HA) --
  snprintf(uid, sizeof(uid), "%s_wifi_rssi", devId);
  snprintf(stBuf, sizeof(stBuf), "%s/sensor/wifi/rssi", prefix);
  disc("sensor", uid, "WiFi RSSI", stBuf, "dBm", "signal_strength", "measurement", "diagnostic");

  snprintf(uid, sizeof(uid), "%s_wifi_ssid", devId);
  snprintf(stBuf, sizeof(stBuf), "%s/sensor/wifi/ssid", prefix);
  disc("sensor", uid, "WiFi SSID", stBuf, "", "", "", "diagnostic");

  snprintf(uid, sizeof(uid), "%s_wifi_ip", devId);
  snprintf(stBuf, sizeof(stBuf), "%s/sensor/wifi/ip", prefix);
  disc("sensor", uid, "WiFi IP", stBuf, "", "", "", "diagnostic");

  // Publish WiFi stats now
  {
    char t[96], v[32];
    snprintf(t, sizeof(t), "%s/sensor/wifi/rssi", prefix);
    snprintf(v, sizeof(v), "%d", (int)WiFi.RSSI());
    _client.publish(t, v);

    snprintf(t, sizeof(t), "%s/sensor/wifi/ssid", prefix);
    _client.publish(t, WiFi.SSID().c_str());

    snprintf(t, sizeof(t), "%s/sensor/wifi/ip", prefix);
    _client.publish(t, WiFi.localIP().toString().c_str());
  }


  // -- Heap + PSRAM diagnostics --
  snprintf(uid, sizeof(uid), "%s_heap_free", devId);
  snprintf(stBuf, sizeof(stBuf), "%s/sensor/heap/free", prefix);
  disc("sensor", uid, "Free Heap", stBuf, "B", "", "measurement", "diagnostic");

  snprintf(uid, sizeof(uid), "%s_heap_min_free", devId);
  snprintf(stBuf, sizeof(stBuf), "%s/sensor/heap/min_free", prefix);
  disc("sensor", uid, "Min Free Heap", stBuf, "B", "", "measurement", "diagnostic");

  snprintf(uid, sizeof(uid), "%s_heap_max_alloc_block", devId);
  snprintf(stBuf, sizeof(stBuf), "%s/sensor/heap/max_alloc_block", prefix);
  disc("sensor", uid, "Max Alloc Block", stBuf, "B", "", "measurement", "diagnostic");

  snprintf(uid, sizeof(uid), "%s_heap_psram_free", devId);
  snprintf(stBuf, sizeof(stBuf), "%s/sensor/heap/psram_free", prefix);
  disc("sensor", uid, "Free PSRAM", stBuf, "B", "", "measurement", "diagnostic");

  snprintf(uid, sizeof(uid), "%s_uptime", devId);
  snprintf(stBuf, sizeof(stBuf), "%s/sensor/uptime", prefix);
  disc("sensor", uid, "Uptime", stBuf, "s", "duration", "total_increasing", "diagnostic");

  // Publish first heap sample immediately so HA has values on discovery.
  publishHeapStats();

  Log::info(TAG, "HA discovery published");
}

// ---------------------------------------------------------------------------

// Publish free heap, min free heap, max alloc block, free PSRAM, and
// uptime to <prefix>/sensor/{heap,uptime}/*. Called on a 5 min timer
// from loop() and once from publishDiscovery() for immediate HA
// visibility. Last sampled free heap is cached in _lastHeapFree for
// alert tagging via currentFreeHeap(). Routes through the static
// publish() bus so cellular-only deployments get the same telemetry
// (publish() picks WiFi or cellular forwarder based on s_fallbackPublishing).
void MQTTClient::publishHeapStats() {
  if (!_client.connected() && !s_fallbackPublishing) return;

  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";

  uint32_t freeHeap   = ESP.getFreeHeap();
  uint32_t minFree    = ESP.getMinFreeHeap();
  uint32_t maxAlloc   = ESP.getMaxAllocHeap();
  uint32_t freePsram  = 0;
#if defined(BOARD_HAS_PSRAM)
  if (psramFound()) freePsram = ESP.getFreePsram();
#endif
  _lastHeapFree = freeHeap;

  char topic[96], value[16];

  snprintf(topic, sizeof(topic), "%s/sensor/heap/free", prefix);
  snprintf(value, sizeof(value), "%lu", (unsigned long)freeHeap);
  publish(topic, value);

  snprintf(topic, sizeof(topic), "%s/sensor/heap/min_free", prefix);
  snprintf(value, sizeof(value), "%lu", (unsigned long)minFree);
  publish(topic, value);

  snprintf(topic, sizeof(topic), "%s/sensor/heap/max_alloc_block", prefix);
  snprintf(value, sizeof(value), "%lu", (unsigned long)maxAlloc);
  publish(topic, value);

  snprintf(topic, sizeof(topic), "%s/sensor/heap/psram_free", prefix);
  snprintf(value, sizeof(value), "%lu", (unsigned long)freePsram);
  publish(topic, value);

  snprintf(topic, sizeof(topic), "%s/sensor/uptime", prefix);
  snprintf(value, sizeof(value), "%lu", (unsigned long)(millis() / 1000));
  publish(topic, value);

  // WiFi diagnostics. Only meaningful when WiFi is the active transport;
  // cellular-only deployments skip these. publishDiscovery seeds them once
  // at MQTT connect, but with a stable session there is no further refresh,
  // so dashboards drift stale. Re-publish on the same 5 min trigger as heap.
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(topic, sizeof(topic), "%s/sensor/wifi/rssi", prefix);
    snprintf(value, sizeof(value), "%d", (int)WiFi.RSSI());
    publish(topic, value);

    snprintf(topic, sizeof(topic), "%s/sensor/wifi/ssid", prefix);
    publish(topic, WiFi.SSID().c_str());

    snprintf(topic, sizeof(topic), "%s/sensor/wifi/ip", prefix);
    publish(topic, WiFi.localIP().toString().c_str());
  }

  _lastHeapPublishMs = millis();
}

// ---------------------------------------------------------------------------

// Publish a retained JSON blob to <prefix>/info describing the firmware and
// hardware: version, chip model/rev, board flag, MAC, PSRAM presence, build
// timestamp. Called once per successful reconnect from connect(). Retained
// so a fresh MQTT subscriber gets the device metadata immediately without
// waiting for the next boot.
// Compute SHA256 hex digest of a string. Output buffer must be >= 65 bytes.
// in: data, length, output buffer. out: none (writes hex string to out).
static void sha256Hex(const char* data, size_t len, char* out) {
  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const unsigned char*)data, len);
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", hash[i]);
  out[64] = '\0';
}

// Compute SHA256 hex digest of a LittleFS file. Returns empty hash if missing.
// in: file path, output buffer (>= 65 bytes). out: none.
static void sha256File(const char* path, char* out) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    sha256Hex("", 0, out);
    return;
  }
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  uint8_t buf[256];
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    mbedtls_sha256_update(&ctx, buf, n);
  }
  f.close();
  uint8_t hash[32];
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", hash[i]);
  out[64] = '\0';
}

void MQTTClient::publishDeviceInfo() {
  // No transport gate here - publishRetained handles WiFi-vs-cellular
  // routing. Drops only if neither transport is available.
  if (!_client.connected() && !s_fallbackPublishing) return;

  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";

  const char* board =
#if defined(BOARD_OWB_RESCUE)
    "owb-rescue";
#elif defined(BOARD_S3_BARE)
    "s3-bare";
#else
    "owb";
#endif

  esp_chip_info_t chip;
  esp_chip_info(&chip);
  const char* chipModel = "unknown";
  switch (chip.model) {
    case CHIP_ESP32:   chipModel = "esp32";    break;
    case CHIP_ESP32S2: chipModel = "esp32-s2"; break;
    case CHIP_ESP32S3: chipModel = "esp32-s3"; break;
    case CHIP_ESP32C3: chipModel = "esp32-c3"; break;
    default: break;
  }

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  bool psram = false;
#if defined(BOARD_HAS_PSRAM)
  psram = psramFound();
#endif

  // Compute config + script hashes for drift detection. config_hash MUST
  // match what an external consumer gets when it sha256s /config.json
  // raw bytes via /api/config (or any other channel that reads the file).
  // Re-serialising Config::get() produced the compact-canonical form
  // (no whitespace, ArduinoJson default order) which never matched the
  // raw-file hash - drift detection sat broken regardless of when this
  // payload was republished. Hash the file directly so on-disk content
  // is the single source of truth.
  char configHash[65], mainHash[65], rulesHash[65];
  sha256File("/config.json", configHash);
  sha256File("/scripts/main.lua", mainHash);
  sha256File("/scripts/rules.lua", rulesHash);

  char payload[640];
  snprintf(payload, sizeof(payload),
    "{\"firmware_version\":\"%s\","
    "\"hardware_type\":\"%s\","
    "\"board\":\"%s\","
    "\"chip_model\":\"%s\","
    "\"chip_revision\":%d,"
    "\"chip_cores\":%d,"
    "\"mac\":\"%s\","
    "\"psram\":%s,"
    "\"build_time\":\"%s %s\","
    "\"config_hash\":\"%s\","
    "\"scripts_main_hash\":\"%s\","
    "\"scripts_rules_hash\":\"%s\"}",
    FIRMWARE_VERSION,
    chipModel,
    board,
    chipModel,
    chip.revision,
    chip.cores,
    macStr,
    psram ? "true" : "false",
    __DATE__, __TIME__,
    configHash,
    mainHash,
    rulesHash);

  char topic[96];
  snprintf(topic, sizeof(topic), "%s/info", prefix);
  publishRetained(topic, payload);
}

// ---------------------------------------------------------------------------

// Returns the most recently sampled free heap (from the 5 min publish), or
// a live read if the sampler has not run yet. Used by alert tagging.
uint32_t MQTTClient::currentFreeHeap() {
  if (_lastHeapFree > 0) return _lastHeapFree;
  return ESP.getFreeHeap();
}

// ---------------------------------------------------------------------------

// Add a message to the offline queue, dropping oldest if full
void MQTTClient::enqueue(const char* topic, const char* payload) {
  if (_queueCount == MQTT_QUEUE_SIZE) {
    Log::warn(TAG, "Queue full - dropping oldest message");
    _queueHead = (_queueHead + 1) % MQTT_QUEUE_SIZE;
    _queueCount--;
  }

  MQTTMessage& msg = _queue[_queueTail];
  strncpy(msg.topic,   topic,   sizeof(msg.topic)   - 1);
  strncpy(msg.payload, payload, sizeof(msg.payload) - 1);
  msg.topic[sizeof(msg.topic)     - 1] = '\0';
  msg.payload[sizeof(msg.payload) - 1] = '\0';
  msg.valid = true;

  _queueTail = (_queueTail + 1) % MQTT_QUEUE_SIZE;
  _queueCount++;

  char log[64];
  snprintf(log, sizeof(log), "Queued (%d/%d): %s", _queueCount, MQTT_QUEUE_SIZE, topic);
  Log::info(TAG, log);
}

// ---------------------------------------------------------------------------

// Send all queued messages after reconnecting
void MQTTClient::flushQueue() {
  while (_queueCount > 0) {
    MQTTMessage& msg = _queue[_queueHead];
    if (msg.valid) {
      _client.publish(msg.topic, msg.payload);
      msg.valid = false;
      char log[64];
      snprintf(log, sizeof(log), "Flushed queued: %s", msg.topic);
      Log::info(TAG, log);
    }
    _queueHead = (_queueHead + 1) % MQTT_QUEUE_SIZE;
    _queueCount--;
  }
}

// ---------------------------------------------------------------------------

// Return whether the MQTT client is currently connected
bool MQTTClient::connected() {
  // _client is the WiFi-path PubSubClient only. On cellular fallback the
  // MQTT session runs through the forwarder, so s_fallbackPublishing is
  // the connected signal there - same pattern the publish guards use.
  return _client.connected() || s_fallbackPublishing;
}

// Return the wall-clock time of the last successful publish
time_t MQTTClient::lastPublishTime() {
  return _lastPublishTime;
}

// ---------------------------------------------------------------------------
// Per-device mTLS client certificate - NVS storage
// ---------------------------------------------------------------------------

// Store PEM-encoded client cert and/or key in NVS.
// Pass nullptr or empty string to skip a half (lets cert + key arrive in
// separate cli/cert.set messages). Returns false on size violation or
// NVS write failure.
// in:  certPEM, keyPEM (PEM strings or nullptr/empty to skip)
// out: true on success
bool MQTTClient::storeClientCert(const char* certPEM, const char* keyPEM) {
  Preferences prefs;
  if (!prefs.begin(CERT_NS, false)) {
    Log::error(TAG, "NVS open failed (rw)");
    return false;
  }
  bool ok = true;
  if (certPEM && *certPEM) {
    size_t len = strlen(certPEM);
    if (len >= CERT_MAX_LEN) {
      Log::error(TAG, "cert too large for NVS");
      ok = false;
    } else if (!prefs.putString(CERT_KEY_CERT, certPEM)) {
      Log::error(TAG, "NVS putString(cert) failed");
      ok = false;
    }
  }
  if (ok && keyPEM && *keyPEM) {
    size_t len = strlen(keyPEM);
    if (len >= CERT_MAX_LEN) {
      Log::error(TAG, "key too large for NVS");
      ok = false;
    } else if (!prefs.putString(CERT_KEY_KEY, keyPEM)) {
      Log::error(TAG, "NVS putString(key) failed");
      ok = false;
    }
  }
  prefs.end();
  return ok;
}

// Load client cert + key from NVS into caller-provided buffers.
// Both buffers must be at least CERT_MAX_LEN bytes.
// in:  cert, key buffers (>= maxLen bytes each), maxLen
// out: true only if BOTH cert and key are present and fit in buffers
bool MQTTClient::loadClientCert(char* cert, char* key, size_t maxLen) {
  if (!cert || !key || maxLen < CERT_MAX_LEN) return false;
  Preferences prefs;
  if (!prefs.begin(CERT_NS, true)) return false;
  size_t certLen = prefs.getString(CERT_KEY_CERT, cert, maxLen);
  size_t keyLen  = prefs.getString(CERT_KEY_KEY,  key,  maxLen);
  prefs.end();
  return certLen > 0 && keyLen > 0;
}

// Erase client cert + key from NVS. Safe to call when absent.
// Fires the cert-cleared hook after the rows are gone so cellular (or
// any other transport) can drop its cached upload + active session.
// out: true if both keys were cleared (or were already absent)
bool MQTTClient::clearClientCert() {
  Preferences prefs;
  if (!prefs.begin(CERT_NS, false)) return false;
  prefs.remove(CERT_KEY_CERT);
  prefs.remove(CERT_KEY_KEY);
  prefs.end();
  if (_onCertClearedHook) _onCertClearedHook();
  return true;
}

// Register a callback fired from clearClientCert. Idempotent; pass
// nullptr to drop the hook. See header for full contract.
void MQTTClient::setOnClientCertCleared(std::function<void()> fn) {
  _onCertClearedHook = fn;
}

// True if both cert and key are present in NVS (non-zero length).
bool MQTTClient::hasClientCert() {
  Preferences prefs;
  if (!prefs.begin(CERT_NS, true)) return false;
  // Stored as strings via putString - isKey is enough. getBytesLength
  // is for NVS_TYPE_BLOB and logs an error when queried on a string key.
  bool ok = prefs.isKey(CERT_KEY_CERT) && prefs.isKey(CERT_KEY_KEY);
  prefs.end();
  return ok;
}

// Parse stored cert PEM and fill caller-allocated buffers with metadata.
// Never touches the private key. Uses mbedtls directly (already linked
// for TLS), no extra deps.
// in:  cn, serial, notBefore, notAfter buffers (>= maxLen each), maxLen >= 128
// out: true if cert loaded and parsed, false if missing or malformed
bool MQTTClient::getCertInfo(char* cn, char* serial, char* notBefore, char* notAfter, size_t maxLen) {
  if (!cn || !serial || !notBefore || !notAfter || maxLen < 64) return false;

  char* cert = (char*)malloc(CERT_MAX_LEN);
  if (!cert) return false;

  Preferences prefs;
  bool ok = false;
  if (prefs.begin(CERT_NS, true)) {
    size_t got = prefs.getString(CERT_KEY_CERT, cert, CERT_MAX_LEN);
    prefs.end();
    if (got > 0) ok = true;
  }
  if (!ok) { free(cert); return false; }

  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  int rc = mbedtls_x509_crt_parse(&crt, (const unsigned char*)cert, strlen(cert) + 1);
  free(cert);
  if (rc != 0) {
    mbedtls_x509_crt_free(&crt);
    return false;
  }

  // Subject DN - mbedtls returns "CN=foo, O=bar, ..." - extract CN
  char subj[256];
  mbedtls_x509_dn_gets(subj, sizeof(subj), &crt.subject);
  const char* cnp = strstr(subj, "CN=");
  if (cnp) {
    cnp += 3;
    size_t i = 0;
    while (*cnp && *cnp != ',' && i < maxLen - 1) cn[i++] = *cnp++;
    cn[i] = '\0';
  } else {
    snprintf(cn, maxLen, "(no CN)");
  }

  // Serial - hex bytes joined
  char* sp = serial;
  char* se = serial + maxLen - 1;
  for (size_t i = 0; i < crt.serial.len && sp + 2 < se; i++) {
    sp += sprintf(sp, "%02x", crt.serial.p[i]);
  }
  *sp = '\0';

  // Validity - YYYY-MM-DDTHH:MM:SSZ
  snprintf(notBefore, maxLen, "%04d-%02d-%02dT%02d:%02d:%02dZ",
           crt.valid_from.year, crt.valid_from.mon, crt.valid_from.day,
           crt.valid_from.hour, crt.valid_from.min, crt.valid_from.sec);
  snprintf(notAfter, maxLen, "%04d-%02d-%02dT%02d:%02d:%02dZ",
           crt.valid_to.year, crt.valid_to.mon, crt.valid_to.day,
           crt.valid_to.hour, crt.valid_to.min, crt.valid_to.sec);

  mbedtls_x509_crt_free(&crt);
  return true;
}
