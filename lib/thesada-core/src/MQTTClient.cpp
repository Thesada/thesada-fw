// thesada-fw - MQTTClient.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "MQTTClient.h"
#include "Config.h"
#include "EventBus.h"
#include "WiFiManager.h"
#include "Log.h"
#include "Shell.h"
#include "OTAUpdate.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <esp_chip_info.h>
#ifdef ENABLE_ETH
  #include <ETH.h>
#endif

// NVS namespace + keys for per-device mTLS client certificate.
// Kept separate from Config so cert survives factory reset of config.json
// and is never exposed via /api/config or config.dump.
static const char* CERT_NS        = "thesada-tls";
static const char* CERT_KEY_CERT  = "client_cert";
static const char* CERT_KEY_KEY   = "client_key";
static constexpr size_t CERT_MAX_LEN = 4000;  // ESP32 NVS blob size limit

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
  // arduino-esp32 (espressif32@6.x) uses the 5-arg form without RNG.
  rc = mbedtls_pk_parse_key(&pk,
                            (const unsigned char*)key, strlen(key) + 1,
                            nullptr, 0);
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
DeferredCLI      MQTTClient::_deferred = { "", nullptr, 0, false };
uint32_t         MQTTClient::_lastSuccessMs    = 0;
uint32_t         MQTTClient::_connectedSinceMs = 0;
bool             MQTTClient::_insecureFallback = false;
uint32_t         MQTTClient::_lastHeapPublishMs = 0;
uint32_t         MQTTClient::_lastHeapFree      = 0;
bool             MQTTClient::_reinitPending     = false;

char             MQTTClient::_rxRing[MQTTClient::RX_RING_SIZE][96] = {};
uint32_t         MQTTClient::_rxRingTs[MQTTClient::RX_RING_SIZE]    = {};
uint8_t          MQTTClient::_rxRingHead  = 0;
uint8_t          MQTTClient::_rxRingCount = 0;

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

  _client.setServer(host, port);
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
    Log::warn(TAG, "No /ca.crt - TLS without cert verification");
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

    // Store prefix length so the callback can extract the command name.
    static char _cliPrefix[64];
    snprintf(_cliPrefix, sizeof(_cliPrefix), "%s/cli/", prefix);
    static size_t _cliPrefixLen = strlen(_cliPrefix);

    MQTTClient::subscribe(cliTopic, [](const char* topic, const char* payload) {
      // Defer CLI command to loop() - executing inside the PubSubClient
      // callback blocks keepalive and causes disconnects on slow operations
      // (LittleFS writes, config reload, etc).
      if (_deferred.pending) {
        Log::warn("MQTT", "CLI busy - command dropped");
        return;
      }
      strncpy(_deferred.topic, topic, sizeof(_deferred.topic) - 1);
      _deferred.topic[sizeof(_deferred.topic) - 1] = '\0';

      // Copy payload to heap (preserves newlines for file.write)
      size_t plen = payload ? strlen(payload) : 0;
      if (_deferred.payload) { free(_deferred.payload); _deferred.payload = nullptr; }
      if (plen > 0) {
        _deferred.payload = (char*)malloc(plen + 1);
        if (_deferred.payload) {
          memcpy(_deferred.payload, payload, plen);
          _deferred.payload[plen] = '\0';
          _deferred.length = plen;
        }
      } else {
        _deferred.payload = nullptr;
        _deferred.length = 0;
      }
      _deferred.pending = true;
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

    // Publish online status (retained) - LWT handles offline
    _client.publish(availTopic, "online", true);
    resubscribeAll();
    publishDiscovery();
    publishDeviceInfo();
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
  // Process deferred CLI command outside the PubSubClient callback context.
  if (_deferred.pending) {
    processDeferredCLI();
  }

  // Deferred reconnect after reinitSubscriptions() - runs on a clean stack
  // so TLS handshake has enough room (~10KB for mbedtls).
  if (_reinitPending) {
    _reinitPending = false;
    JsonObject  cfgR = Config::get();
    const char* host = cfgR["mqtt"]["broker"] | "";
    uint16_t    port = cfgR["mqtt"]["port"]   | 8883;
    _client.setServer(host, port);
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

    // Periodic heap telemetry. Runs on the MQTT loop
    // cadence but only when the 5 min interval has elapsed, so we get no
    // more than 288 publishes/day per metric.
    if (_lastHeapPublishMs == 0 || (now - _lastHeapPublishMs) >= HEAP_PUBLISH_MS) {
      publishHeapStats();
    }

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
  enqueue(topic, payload);
}

// Publish a retained message (for HA discovery configs)
void MQTTClient::publishRetained(const char* topic, const char* payload) {
  if (_client.connected()) {
    _client.publish(topic, (const uint8_t*)payload, strlen(payload), true);
  }
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
}

// ---------------------------------------------------------------------------

// Route incoming messages to matching subscription callbacks
void MQTTClient::onMessage(char* topic, uint8_t* payload, unsigned int length) {
  // Null-terminate the payload - heap-allocated to avoid stack overflow on WROOM-32.
  size_t bufSize = length + 1;
  char* payloadBuf = (char*)malloc(bufSize);
  if (!payloadBuf) return;
  memcpy(payloadBuf, payload, length);
  payloadBuf[length] = '\0';

  // Debug RX ring: capture every received topic so `net.mqtt rx` can show it.
  // Helps narrow broker-side delivery vs client-side dispatch when a
  // subscription is active but its callback never fires.
  strncpy(_rxRing[_rxRingHead], topic, sizeof(_rxRing[0]) - 1);
  _rxRing[_rxRingHead][sizeof(_rxRing[0]) - 1] = '\0';
  _rxRingTs[_rxRingHead] = millis();
  _rxRingHead = (_rxRingHead + 1) % RX_RING_SIZE;
  if (_rxRingCount < RX_RING_SIZE) _rxRingCount++;

  // Route to matching subscription callbacks (exact or wildcard # match).
  for (uint8_t i = 0; i < _subCount; i++) {
    if (!_subs[i].active) continue;
    // Check for trailing /# wildcard
    size_t slen = strlen(_subs[i].topic);
    if (slen >= 2 && _subs[i].topic[slen - 1] == '#' && _subs[i].topic[slen - 2] == '/') {
      if (strncmp(_subs[i].topic, topic, slen - 1) == 0) {
        _subs[i].callback(topic, payloadBuf);
      }
    } else if (strcmp(_subs[i].topic, topic) == 0) {
      _subs[i].callback(topic, payloadBuf);
    }
  }
  free(payloadBuf);
}

// ---------------------------------------------------------------------------

// Process a deferred CLI command (runs in loop(), not in PubSubClient callback)
void MQTTClient::processDeferredCLI() {
  _deferred.pending = false;

  // Extract command from topic after <prefix>/cli/
  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  char cliPrefix[64];
  snprintf(cliPrefix, sizeof(cliPrefix), "%s/cli/", prefix);
  size_t prefixLen = strlen(cliPrefix);

  const char* cmd = _deferred.topic + prefixLen;
  if (strlen(cmd) == 0 || strcmp(cmd, "response") == 0) goto cleanup;

  {
    const char* payload = _deferred.payload;
    size_t plen = _deferred.length;

    // Special case: fs.write / fs.append / file.write (legacy alias)
    // First line of payload is the path, rest is content.
    // fs.write truncates (mode "w"), fs.append appends (mode "a").
    if ((strcmp(cmd, "fs.write") == 0 || strcmp(cmd, "file.write") == 0 ||
         strcmp(cmd, "fs.append") == 0) && payload && plen > 0) {
      const char* mode = (strcmp(cmd, "fs.append") == 0) ? "a" : "w";
      const char* nl = strchr(payload, '\n');
      JsonDocument resp;
      resp["cmd"] = cmd;
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

        File f = LittleFS.open(path, mode);
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

        File f = LittleFS.open(path, "r");
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

    // Build command line: "<cmd> <payload>"
    char line[1024];
    if (payload && plen > 0) {
      snprintf(line, sizeof(line), "%s %s", cmd, payload);
    } else {
      snprintf(line, sizeof(line), "%s", cmd);
    }

    // Execute through Shell and collect output.
    JsonDocument resp;
    resp["cmd"] = cmd;
    resp["ok"]  = true;
    JsonArray output = resp["output"].to<JsonArray>();

    Shell::execute(line, [&output](const char* outLine) {
      output.add(outLine);
    });

    // Re-read prefix after execute (config.reload may have invalidated it)
    JsonObject cfgAfter = Config::get();
    const char* pfxAfter = cfgAfter["mqtt"]["topic_prefix"] | "thesada/node";
    char respTopic[64];
    snprintf(respTopic, sizeof(respTopic), "%s/cli/response", pfxAfter);
    size_t bufSz = _bufferOut > 0 ? _bufferOut : 4096;
    char* respPayload = (char*)malloc(bufSz);
    if (respPayload) {
      serializeJson(resp, respPayload, bufSz);
      MQTTClient::publish(respTopic, respPayload);
      free(respPayload);
    }
  }

cleanup:
  if (_deferred.payload) { free(_deferred.payload); _deferred.payload = nullptr; }
  _deferred.length = 0;
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

  static char _cliPrefix[64];
  snprintf(_cliPrefix, sizeof(_cliPrefix), "%s/cli/", prefix);
  static size_t _cliPrefixLen = strlen(_cliPrefix);

  MQTTClient::subscribe(cliTopic, [](const char* topic, const char* payload) {
    if (_deferred.pending) {
      Log::warn("MQTT", "CLI busy - command dropped");
      return;
    }
    strncpy(_deferred.topic, topic, sizeof(_deferred.topic) - 1);
    _deferred.topic[sizeof(_deferred.topic) - 1] = '\0';

    size_t plen = payload ? strlen(payload) : 0;
    if (_deferred.payload) { free(_deferred.payload); _deferred.payload = nullptr; }
    if (plen > 0) {
      _deferred.payload = (char*)malloc(plen + 1);
      if (_deferred.payload) {
        memcpy(_deferred.payload, payload, plen);
        _deferred.payload[plen] = '\0';
        _deferred.length = plen;
      }
    } else {
      _deferred.payload = nullptr;
      _deferred.length = 0;
    }
    _deferred.pending = true;
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
    _client.publish(topic, (const uint8_t*)payload, strlen(payload), true);
    // Drain TCP buffer between retained publishes. Without this, small
    // MQTT buffers (CYD 1024B) overflow and spam EAGAIN socket errors.
    _client.loop();
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

#ifdef ENABLE_ETH
  // -- Ethernet diagnostics (disabled by default in HA) --
  snprintf(uid, sizeof(uid), "%s_eth_ip", devId);
  snprintf(stBuf, sizeof(stBuf), "%s/sensor/eth/ip", prefix);
  disc("sensor", uid, "Ethernet IP", stBuf, "", "", "", "diagnostic");

  snprintf(uid, sizeof(uid), "%s_eth_speed", devId);
  snprintf(stBuf, sizeof(stBuf), "%s/sensor/eth/speed", prefix);
  disc("sensor", uid, "Ethernet Speed", stBuf, "Mbps", "", "measurement", "diagnostic");

  snprintf(uid, sizeof(uid), "%s_eth_mac", devId);
  snprintf(stBuf, sizeof(stBuf), "%s/sensor/eth/mac", prefix);
  disc("sensor", uid, "Ethernet MAC", stBuf, "", "", "", "diagnostic");

  // Publish ETH stats now
  if (ETH.linkUp()) {
    char t[96], v[32];
    snprintf(t, sizeof(t), "%s/sensor/eth/ip", prefix);
    _client.publish(t, ETH.localIP().toString().c_str());

    snprintf(t, sizeof(t), "%s/sensor/eth/speed", prefix);
    snprintf(v, sizeof(v), "%d", ETH.linkSpeed());
    _client.publish(t, v);

    snprintf(t, sizeof(t), "%s/sensor/eth/mac", prefix);
    _client.publish(t, ETH.macAddress().c_str());
  }
#endif

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

  // Publish first heap sample immediately so HA has values on discovery.
  publishHeapStats();

  Log::info(TAG, "HA discovery published");
}

// ---------------------------------------------------------------------------

// Publish free heap, min free heap, max alloc block, and free PSRAM to
// <prefix>/sensor/heap/*. Called on a 5 min timer from loop() and once from
// publishDiscovery() for immediate HA visibility. Last sampled free heap is
// cached in _lastHeapFree for alert tagging via currentFreeHeap().
void MQTTClient::publishHeapStats() {
  if (!_client.connected()) return;

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
  _client.publish(topic, value);

  snprintf(topic, sizeof(topic), "%s/sensor/heap/min_free", prefix);
  snprintf(value, sizeof(value), "%lu", (unsigned long)minFree);
  _client.publish(topic, value);

  snprintf(topic, sizeof(topic), "%s/sensor/heap/max_alloc_block", prefix);
  snprintf(value, sizeof(value), "%lu", (unsigned long)maxAlloc);
  _client.publish(topic, value);

  snprintf(topic, sizeof(topic), "%s/sensor/heap/psram_free", prefix);
  snprintf(value, sizeof(value), "%lu", (unsigned long)freePsram);
  _client.publish(topic, value);

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
  if (!_client.connected()) return;

  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";

  const char* board =
#if defined(BOARD_OWB_RESCUE)
    "owb-rescue";
#elif defined(BOARD_S3_BARE)
    "s3-bare";
#elif defined(BOARD_CYD)
    "cyd";
#elif defined(BOARD_WROOM)
    "wroom";
#elif defined(BOARD_ETH)
    "eth";
#else
    "unknown";
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

  // Compute config + script hashes for drift detection
  char configHash[65], mainHash[65], rulesHash[65];
  {
    String cfgJson;
    serializeJson(Config::get(), cfgJson);
    sha256Hex(cfgJson.c_str(), cfgJson.length(), configHash);
  }
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
  _client.publish(topic, payload, true);
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
  return _client.connected();
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
// out: true if both keys were cleared (or were already absent)
bool MQTTClient::clearClientCert() {
  Preferences prefs;
  if (!prefs.begin(CERT_NS, false)) return false;
  prefs.remove(CERT_KEY_CERT);
  prefs.remove(CERT_KEY_KEY);
  prefs.end();
  return true;
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
