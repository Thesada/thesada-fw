// thesada-fw - MQTTClient.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "MQTTClient.h"
#include <thesada_config.h>
#include "Config.h"
#include "Secret.h"
#include "cli_payload.h"
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

// NVS namespace separate from Config: cert survives factory reset and never
// appears in /api/config or config.dump.
static const char* CERT_NS        = "thesada-tls";
static const char* CERT_KEY_CERT  = "client_cert";
static const char* CERT_KEY_KEY   = "client_key";

// Separate namespace from cert: a cert wipe must not reset the reboot guard,
// and vice versa.
static const char* BOOT_NS        = "thesada-boot";
static const char* RB_KEY_COUNT   = "mqtt_reboots";  // uint8: consecutive
                                                     //   exhaustion reboots
static const char* RB_KEY_EPOCH   = "mqtt_rb_at";    // uint32: epoch (s)
                                                     //   of the last one
static const char* RB_KEY_LG      = "mqtt_lg";       // string: last-good
                                                     //   critical mqtt cfg (JSON)
static const char* RB_KEY_LGSET   = "mqtt_lg_set";   // uint8: 1 once a snapshot
                                                     //   exists
static const char* RB_KEY_RBCFG   = "mqtt_rb_cfg";   // string: critical cfg live
                                                     //   when a reboot fired

// Buffer for the connection-critical mqtt subset (broker/port/user/password)
// serialized as JSON. Comfortably fits a hostname + creds.
static constexpr size_t LG_MAX_LEN = 384;

// Consecutive exhaustion reboots older than this age out: a one-off
// outage weeks apart never accumulates toward a halt. Only applied when
// the system clock is set.
static constexpr uint32_t RB_STALE_WINDOW_S = 6UL * 3600UL;

// Cellular installs a hook so its cert cache and any live SMCONN session
// drop when the cert is cleared. nullptr when unused.
static std::function<void()> _onCertClearedHook = nullptr;

static const char* TAG = "MQTT";

#ifdef MQTT_TLS
WiFiClientSecure MQTTClient::_wifiClient;
// Buffers held live across the TLS session; WiFiClientSecure stores the
// raw pointer and dereferences it during the handshake.
static char*     _clientCert    = nullptr;
static char*     _clientKey     = nullptr;
static bool      _mtlsActive    = false;
static bool      _mtlsWasActive = false;  // tracks whether last connect() used mTLS; need to clear WiFiClientSecure on fallback

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
// TLS stack and break every future reconnect until restart. Also rejects
// a mismatched pair (cert A + key B): each parses fine on its own, so
// without the explicit pair check the mismatch only surfaces later as an
// opaque "TLS handshake failed" instead of an immediate cert.set error.
// in: cert (PEM string), key (PEM string).
// out: true if both parse AND the key is the private half of the cert's
//      public key.
static bool validateClientCertKey(const char* cert, const char* key) {
  if (!cert || !key || !*cert || !*key) return false;

  mbedtls_x509_crt crt;
  mbedtls_pk_context pk;
  mbedtls_x509_crt_init(&crt);
  mbedtls_pk_init(&pk);

  // crt must stay live until after the pair check - it owns the public
  // key that mbedtls_pk_check_pair compares the private key against.
  bool ok = false;
  do {
    if (mbedtls_x509_crt_parse(&crt, (const unsigned char*)cert, strlen(cert) + 1) != 0)
      break;
    // mbedtls 3.x (pioarduino / IDF 5.x) added RNG callback args to both
    // pk_parse_key and pk_check_pair; 2.x has the shorter forms.
#if MBEDTLS_VERSION_MAJOR >= 3
    if (mbedtls_pk_parse_key(&pk, (const unsigned char*)key, strlen(key) + 1,
                             nullptr, 0, nullptr, nullptr) != 0)
      break;
    if (mbedtls_pk_check_pair(&crt.pk, &pk, nullptr, nullptr) != 0)
      break;
#else
    if (mbedtls_pk_parse_key(&pk, (const unsigned char*)key, strlen(key) + 1,
                             nullptr, 0) != 0)
      break;
    if (mbedtls_pk_check_pair(&crt.pk, &pk) != 0)
      break;
#endif
    ok = true;
  } while (0);

  mbedtls_pk_free(&pk);
  mbedtls_x509_crt_free(&crt);
  return ok;
}

// Raw char* (not Arduino String) so heap_caps_malloc(MALLOC_CAP_SPIRAM)
// can route it to PSRAM, keeping ~2 KB off the internal heap.
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
bool             MQTTClient::_rebootHalted  = false;
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

// When true, publish() routes to the cellular forwarder on WiFi disconnect
// instead of enqueueing. See header.
static bool s_fallbackPublishing = false;

// Mirrors new subscriptions onto the cellular MQTT session when installed.
static std::function<void(const char*)> s_subForwarder;

// Routes publishes to cellular when fallback is active. Returns false if
// cellular is not ready (message drops).
static std::function<bool(const char*, const char*, bool)> s_pubForwarder;

// Timestamp of cellular handoff; heap-stats firing defers 30 s to avoid
// crowding the modem AT bus during the initial post-ACTIVE burst.
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

// Initialize MQTT: load config, TLS certs, set up subscriptions, connect.
// in: none (reads Config, LittleFS /ca.crt, NVS cert namespace).
// out: none.
void MQTTClient::begin() {
  // Open the NVS namespace rw once so first-boot devices don't get
  // "nvs_open failed: NOT_FOUND" spam from the subsequent read-only calls.
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

  // Copy into persistent buffer - see _brokerHost comment above.
  strncpy(_brokerHost, host, sizeof(_brokerHost) - 1);
  _brokerHost[sizeof(_brokerHost) - 1] = '\0';
  _client.setServer(_brokerHost, port);
  _client.setKeepAlive(60);
  _client.setBufferSize(_bufferIn);
  _client.setCallback(onMessage);

#ifdef MQTT_TLS
  // Keep well under the 30 s hardware watchdog.
  // v2.x setTimeout takes seconds; v3.x takes milliseconds. Setting both
  // handles the difference. setHandshakeTimeout is always in seconds.
  _wifiClient.setTimeout(10);
  _wifiClient.setHandshakeTimeout(10);  // ssl handshake, always seconds

  // Load CA cert from LittleFS; fall back to baked PROGMEM bundle; last
  // resort setInsecure. Prefer PSRAM when available (see _caCert comment).
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
    // Same roots as OTA; a stripped LittleFS must not silently go insecure.
    // setInsecure below is a last resort for a misbuilt PROGMEM bundle.
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

  for (int i = 0; i < MQTT_MAX_SUBS; i++) {
    _subs[i].active = false;
  }

  EventBus::subscribe("alert", [](JsonObject data) {
    JsonObject  cfg    = Config::get();
    const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
    char topic[64];
    snprintf(topic, sizeof(topic), "%s/alert", prefix);
    char payload[256];
    serializeJson(data, payload, sizeof(payload));
    MQTTClient::publish(topic, payload);
  });

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


  connect();
}

// ---------------------------------------------------------------------------

// Read persisted broker-exhaustion reboot count. A streak older than
// RB_STALE_WINDOW_S (clock must be set) ages out and returns 0.
// out: effective consecutive count (0 if none or stale).
static uint8_t mqttRebootCount() {
  Preferences prefs;
  if (!prefs.begin(BOOT_NS, true)) return 0;
  uint8_t  count = prefs.getUChar(RB_KEY_COUNT, 0);
  uint32_t epoch = prefs.getUInt(RB_KEY_EPOCH, 0);
  prefs.end();

  time_t now = time(nullptr);
  if (count > 0 && now > 1700000000L && epoch > 0 &&
      (uint32_t)now - epoch > RB_STALE_WINDOW_S) {
    return 0;  // streak aged out
  }
  return count;
}

// Persist newCount and current epoch (0 if clock unset; stale check skips).
static void mqttRebootCountBump(uint8_t newCount) {
  Preferences prefs;
  if (!prefs.begin(BOOT_NS, false)) return;
  prefs.putUChar(RB_KEY_COUNT, newCount);
  time_t now = time(nullptr);
  prefs.putUInt(RB_KEY_EPOCH, (now > 1700000000L) ? (uint32_t)now : 0);
  prefs.end();
}

// Clear the exhaustion-reboot counter. Reads before writing so a steady-
// state connect does not rewrite NVS every cycle.
static void mqttRebootCountClear() {
  Preferences prefs;
  if (!prefs.begin(BOOT_NS, false)) return;
  if (prefs.getUChar(RB_KEY_COUNT, 0) != 0) {
    prefs.remove(RB_KEY_COUNT);
    prefs.remove(RB_KEY_EPOCH);
  }
  prefs.end();
}

// Record / clear the critical config that was live when an exhaustion reboot
// fired. The rollback decision keys off this so it restores only the config
// that actually rebooted-without-connecting, never an unrelated later edit.
static void mqttWriteRbCfg(const char* json) {
  Preferences prefs;
  if (!prefs.begin(BOOT_NS, false)) return;
  prefs.putString(RB_KEY_RBCFG, json);
  prefs.end();
}

static void mqttClearRbCfg() {
  Preferences prefs;
  if (!prefs.begin(BOOT_NS, false)) return;
  if (prefs.isKey(RB_KEY_RBCFG)) prefs.remove(RB_KEY_RBCFG);
  prefs.end();
}

// Pure rollback decision (no NVS / Config) so it is unit-testable. Roll back
// iff a snapshot exists, the current config is the exact one that rebooted
// without connecting (rbCfg), and it differs from last-good.
bool MQTTClient::rollbackDecision(const char* lg, bool haveLg,
                                  const char* rbCfg, const char* cur) {
  if (!haveLg || !lg || !*lg) return false;      // nothing to fall back to
  if (!rbCfg || !*rbCfg) return false;           // no failing candidate recorded
  if (!cur) return false;
  if (strcmp(cur, rbCfg) != 0) return false;     // current != the config that failed
  if (strcmp(cur, lg) == 0) return false;        // current == last-good
  return true;
}

// Serialize the connection-critical mqtt keys in a fixed order so two identical
// configs yield identical strings. out: bytes written (0 on overflow/failure).
static size_t mqttCriticalJson(char* out, size_t maxLen) {
  JsonObject m = Config::get()["mqtt"];
  JsonDocument d;
  d["broker"]   = m["broker"]   | "";
  d["port"]     = m["port"]     | 0;
  d["user"]     = m["user"]     | "";
  d["password"] = m["password"] | "";
  size_t n = serializeJson(d, out, maxLen);
  return (n > 0 && n < maxLen) ? n : 0;
}

// Snapshot the connection-critical mqtt config as last-good. Writes NVS only
// when it changed (flash wear).
void MQTTClient::snapshotGoodConfig() {
  char cur[LG_MAX_LEN];
  if (mqttCriticalJson(cur, sizeof(cur)) == 0) return;

  Preferences prefs;
  if (!prefs.begin(BOOT_NS, false)) return;
  char prev[LG_MAX_LEN] = {0};
  prefs.getString(RB_KEY_LG, prev, sizeof(prev));
  if (strcmp(cur, prev) != 0) {
    // Both writes must land, else mqtt_lg / mqtt_lg_set diverge; report failure
    // rather than logging a commit that did not persist.
    bool ok = prefs.putString(RB_KEY_LG, cur) > 0;
    ok = (prefs.putUChar(RB_KEY_LGSET, 1) > 0) && ok;
    Log::info(TAG, ok ? "mqtt.config_committed" : "mqtt.config_commit_failed");
  }
  prefs.end();
}

// Restore the last-good snapshot if the still-current critical config is the
// exact one that rebooted without connecting. A merely-offline broker (config
// unchanged) and an unrelated later edit both fall through - see rollbackDecision.
void MQTTClient::rollbackIfUncommitted() {
  Preferences prefs;
  if (!prefs.begin(BOOT_NS, true)) return;
  uint8_t haveLg = prefs.getUChar(RB_KEY_LGSET, 0);
  char lgJson[LG_MAX_LEN] = {0};
  char rbJson[LG_MAX_LEN] = {0};
  if (haveLg) prefs.getString(RB_KEY_LG, lgJson, sizeof(lgJson));
  prefs.getString(RB_KEY_RBCFG, rbJson, sizeof(rbJson));
  prefs.end();

  char cur[LG_MAX_LEN];
  if (mqttCriticalJson(cur, sizeof(cur)) == 0) return;
  if (!rollbackDecision(lgJson, haveLg != 0, rbJson, cur)) return;

  JsonDocument lg;
  if (deserializeJson(lg, lgJson)) return;  // corrupt snapshot: leave config alone

  JsonObject m = Config::get()["mqtt"];
  m["broker"]   = String((const char*)(lg["broker"] | ""));
  m["port"]     = lg["port"] | 0;
  m["user"]     = String((const char*)(lg["user"]     | ""));
  m["password"] = String((const char*)(lg["password"] | ""));
  if (Config::save()) {
    mqttRebootCountClear();                 // fresh reboot budget for the restored config
    mqttClearRbCfg();
    Log::warn(TAG, "mqtt.config_rollback");
  } else {
    Log::error(TAG, "mqtt.config_rollback_save_failed");
  }
}

// ---------------------------------------------------------------------------

// Connect to the MQTT broker. Applies mTLS when a client cert is present.
// Sets LWT so the broker publishes "offline" on disconnect.
// in: none (reads Config + NVS cert namespace). out: none.
void MQTTClient::connect() {
  if (!WiFiManager::connected()) return;

  JsonObject  cfg      = Config::get();
  const char* clientId = cfg["device"]["name"]   | "thesada-node";
  const char* user     = cfg["mqtt"]["user"]      | "";
  char        passwordBuf[Secret::MAX_LEN];
  const char* password = Secret::resolve("mqtt_password", cfg["mqtt"]["password"] | "",
                                         passwordBuf, sizeof(passwordBuf));
  const char* prefix   = cfg["mqtt"]["topic_prefix"] | "thesada/node";

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

  // Load client cert from NVS into module-level buffers. WiFiClientSecure
  // holds the raw pointer for the TLS session lifetime, so buffers must
  // stay live across handshake. Validate via mbedtls before setCertificate:
  // there is no clear-cert API, so a bad pointer breaks every future
  // reconnect until restart.
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
      // Free both on any failure (malloc partial or NVS miss) - a stranded
      // 4 KB buffer starves the next connect attempt. free(nullptr) is safe.
      free(_clientCert); _clientCert = nullptr;
      free(_clientKey);  _clientKey  = nullptr;
      Log::warn(TAG, "mTLS: NVS load failed, falling back to password auth");
    }
  }
  // When dropping from mTLS to password auth, pass nullptr to evict the
  // stale setCertificate pointer - it lingers and breaks future connect()s.
  if (_mtlsWasActive && !_mtlsActive) {
    _wifiClient.setCertificate(nullptr);
    _wifiClient.setPrivateKey(nullptr);
    Log::info(TAG, "mTLS cleared - prior cert pointer reset in WiFiClientSecure");
  }
  _mtlsWasActive = _mtlsActive;
#endif

  // Broker uses CN as username (use_identity_as_username) when mTLS is
  // active - sending user/pass alongside is redundant and confusing.
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
    // Clear the reboot guard: a future failure starts a fresh streak.
    _rebootHalted  = false;
    mqttRebootCountClear();
    mqttClearRbCfg();   // this config connected: it is no longer a failing candidate
    // This config just connected: commit it as the last-good rollback target.
    snapshotGoodConfig();
    _lastSuccessMs    = millis();
    _connectedSinceMs = millis();
    Log::info(TAG, "Connected");

    // TCP keepalive catches dead connections faster than MQTT-level keepalive.
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

    // Reset manifest; modules repopulate it during this session.
    _retainedTopics.clear();
    _manifestPublished    = false;
    _manifestDirty        = false;
    _manifestDirtySinceMs = 0;

    resubscribeAll();
    // force=true: WiFi reconnect must not be blocked by a stale cellular flag.
    publishRetainedSet(true);
    flushQueue();
  } else {
    char err[64];
    snprintf(err, sizeof(err), "Failed (rc=%d) - retry in %lums",
             _client.state(), _retryInterval);
    Log::warn(TAG, err);
    _retryCount++;
    _retryInterval = min(_retryInterval * 2, (uint32_t)RETRY_MAX_MS);

    // mbedtls needs ~30 KB contiguous; retrying without a reboot just
    // fails again. Allow 3 attempts for transient failures first.
    if (_retryCount >= 3 && ESP.getMaxAllocHeap() < 40000) {
      Log::error(TAG, "TLS alloc failed 3x with low heap - rebooting to defrag");
      delay(1000);
      ESP.restart();
    }

    // Broker-misconfiguration safety net. A bad host/port/creds used to
    // reboot every ~30 min forever, leaving the device reachable only
    // briefly each cycle. Instead: after reboot_after_fails attempts,
    // reboot at most max_exhaust_reboots times (a reboot can clear a
    // wedged TLS stack), then stop rebooting and stay alive - serial/web
    // still reachable, MQTT still retrying at capped backoff.
    uint8_t rebootAfter = Config::get()["mqtt"]["reboot_after_fails"] | 30;
    if (_retryCount >= rebootAfter && !_rebootHalted) {
      uint8_t maxReboots = Config::get()["mqtt"]["max_exhaust_reboots"] | 3;
      uint8_t count      = mqttRebootCount();
      if (count < maxReboots) {
        char m[96];
        snprintf(m, sizeof(m),
                 "MQTT failed %ux - reboot %u/%u to recover",
                 (unsigned)rebootAfter, (unsigned)(count + 1),
                 (unsigned)maxReboots);
        Log::error(TAG, m);
        // Record the config that is failing so the boot-time rollback restores
        // exactly this one, not an unrelated later edit.
        char failing[LG_MAX_LEN];
        if (mqttCriticalJson(failing, sizeof(failing)) > 0) mqttWriteRbCfg(failing);
        mqttRebootCountBump(count + 1);
        delay(1000);
        ESP.restart();
      } else {
        // Budget spent. Keep retrying, never reboot again.
        _rebootHalted = true;
        char m[128];
        snprintf(m, sizeof(m),
                 "MQTT failed %ux after %u reboots - staying alive, "
                 "retrying without reboot (check broker config)",
                 (unsigned)rebootAfter, (unsigned)maxReboots);
        Log::error(TAG, m);
      }
    }
  }
}

// ---------------------------------------------------------------------------

// Run PubSubClient::loop() to service keepalive. Safe to call from
// beginAll() between module inits so long init sequences don't drop
// the broker connection.
void MQTTClient::tick() {
  if (_client.connected()) {
    _client.loop();
  }
}

// ---------------------------------------------------------------------------

void MQTTClient::loop() {
  // Reboot if heap stays below floor for HEAP_REBOOT_HOLD_MS: mbedtls OOM
  // mid-handshake wedges the connection with no clean recovery; a fresh
  // boot defragments. Set HEAP_REBOOT_FLOOR_BYTES=0 to disable.
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


  // Transport-agnostic heap telemetry. Defers 30 s after cellular handoff
  // (see s_fallbackStartMs) so the post-ACTIVE AT burst clears first.
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

  // 5 s debounce: late-arriving retained topics (module begin() burst)
  // re-emit without thrashing the broker.
  if (_manifestDirty && _client.connected() &&
      (millis() - _manifestDirtySinceMs) >= 5000) {
    publishRetainedManifest();
  }

  // cert.apply deferred reboot: shell handler publishes its response, then
  // this tick fires the restart. Only reliable way to clear sticky
  // WiFiClientSecure / mbedtls state on a cert swap; remote devices have
  // no USB fallback so cert.apply must self-recover.
  if (_certApplyRebootPending && (int32_t)(millis() - _certApplyRebootAtMs) >= 0) {
    Log::warn(TAG, "cert.apply deferred reboot firing");
    delay(100);
    ESP.restart();
  }

  // Deferred reconnect after reinitSubscriptions(): clean stack gives
  // mbedtls enough room (~10 KB) for the TLS handshake.
  if (_reinitPending) {
    _reinitPending = false;
    JsonObject  cfgR = Config::get();
    const char* host = cfgR["mqtt"]["broker"] | "";
    uint16_t    port = cfgR["mqtt"]["port"]   | 8883;
    // Refresh persistent buffer; re-parsed pool invalidates prior pointer.
    strncpy(_brokerHost, host, sizeof(_brokerHost) - 1);
    _brokerHost[sizeof(_brokerHost) - 1] = '\0';
    _client.setServer(_brokerHost, port);
    _retryInterval = RETRY_MIN_MS;
    connect();
  }

  if (_client.connected()) {
    // NTP has synced since the insecure connect - reconnect with cert
    // validation. Skip if contiguous heap < 40 KB (TLS needs ~30 KB).
    if (_insecureFallback && time(nullptr) > 1700000000 && ESP.getMaxAllocHeap() > 40000) {
      Log::info(TAG, "NTP synced - upgrading to cert-validated connection");
      _connectedSinceMs = 0;  // suppress spurious "lost after 0 seconds" log
      _client.disconnect();
      _wifiClient.stop();
      _retryInterval = RETRY_MIN_MS;
      return;
    }

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
    _lastSuccessMs = millis();

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
  // WiFi MQTT is down. Route to cellular forwarder when active - do NOT
  // enqueue on the WiFi ring or it fills 8/8 and every publish triggers
  // "Queue full - dropping oldest".
  if (s_fallbackPublishing) {
    if (s_pubForwarder) s_pubForwarder(topic, payload, false);
    return;
  }
  enqueue(topic, payload);
}

// Publish retained. Records the topic in the manifest so a device-delete
// sweep can clear it. Routes to the cellular forwarder when WiFi is down.
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

// Dedupe-append topic to the retained manifest. Manifest resets each
// connect() so it always reflects the current session.
// in: topic. out: none.
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

// Publish retained JSON array of all retained topics to
// <prefix>/info/retained_topics so a controller can enumerate and clean
// them on device removal. The manifest topic itself is included so a
// sweep also clears this entry.
// in: none. out: none.
void MQTTClient::publishRetainedManifest() {
  if (!_client.connected() && !s_fallbackPublishing) return;

  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";

  char topic[96];
  snprintf(topic, sizeof(topic), "%s/info/retained_topics", prefix);

  recordRetainedTopic(topic);

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto& t : _retainedTopics) arr.add(t);

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

// Republish retained state: availability, discovery, /info, manifest.
// Session flag prevents repeat on cellular STANDBY->ACTIVE bounces within
// one fallback window; setFallbackPublishing(false) resets it.
// in: force - bypass the session flag.
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

  // Feed keepalive during bulk Lua subscription (>10 s): without this the
  // broker or HAProxy drops the idle TCP connection.
  if (_client.connected()) {
    _client.subscribe(topic);
    _client.loop();
  }

  if (s_subForwarder) s_subForwarder(topic);
}

// ---------------------------------------------------------------------------

// Match and fire subscriptions: exact, /#, /+ wildcard. Shared between
// WiFi (onMessage) and cellular (dispatchInbound) to keep semantics identical.
// in: topic, payload. out: none.
void MQTTClient::matchAndDispatch(const char* topic, const char* payload) {
  strncpy(_rxRing[_rxRingHead], topic, sizeof(_rxRing[0]) - 1);
  _rxRing[_rxRingHead][sizeof(_rxRing[0]) - 1] = '\0';
  _rxRingTs[_rxRingHead] = millis();
  _rxRingHead = (_rxRingHead + 1) % RX_RING_SIZE;
  if (_rxRingCount < RX_RING_SIZE) _rxRingCount++;

  for (uint8_t i = 0; i < _subCount; i++) {
    if (!_subs[i].active) continue;
    size_t slen = strlen(_subs[i].topic);
    if (slen >= 2 && _subs[i].topic[slen - 1] == '#' && _subs[i].topic[slen - 2] == '/') {  // trailing /#
      if (strncmp(_subs[i].topic, topic, slen - 1) == 0) {
        _subs[i].callback(topic, payload);
      }
    }
    // /+ matches only one level (no further '/').
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

// Public entry for Cellular::pumpInbound: routes +SMSUB URCs through the
// same callbacks as the WiFi path.
void MQTTClient::dispatchInbound(const char* topic, const char* payload, size_t length) {
  matchAndDispatch(topic, payload);
  (void)length;  // payload is null-terminated by caller
}

// Cellular bring-up uses this to replay AT+SMSUB for every WiFi-side entry.
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
  // Heap-allocate: stack overflow on WROOM-32 with large payloads.
  size_t bufSize = length + 1;
  char* payloadBuf = (char*)malloc(bufSize);
  if (!payloadBuf) return;
  memcpy(payloadBuf, payload, length);
  payloadBuf[length] = '\0';

  matchAndDispatch(topic, payloadBuf);
  free(payloadBuf);
}

// ---------------------------------------------------------------------------

// Dispatch a CLI command from the Shell deferred ring. cmd was extracted
// from the topic; payload is heap-owned by the std::function capture.
// Binary-protocol commands (fs.write, fs.cat chunked, cert.set) are
// handled as special cases before the Shell::execute fallthrough.
// in: cmd, payload (may be null), plen. out: none.
void MQTTClient::runCli(const char* cmd, const char* payload, size_t plen) {
  if (!cmd || strlen(cmd) == 0) return;

  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";

  // Optional envelope: {"req_id":<id>, "args":<string>}. req_id is echoed
  // on every cli/response for request/response matching on the shared topic.
  // args is unwrapped and replaces payload for all downstream handlers so
  // binary-protocol special cases (fs.write, cert.set) and Shell::execute
  // all see the inner string. Non-envelope payloads fall through untouched.
  // reqDoc stays in scope so reqId and the args view stay valid at every
  // publish site.
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
      const char* argsStr = obj["args"];
      if (argsStr) {
        payload = argsStr;
        plen    = strlen(argsStr);
      }
      // req_id-only envelope = no-arg command; don't pass the JSON as a
      // literal arg to Shell parsers.
      reqIdOnlyPayload = hasReqId && (obj.size() == 1 ||
                                      (argsStr && plen == 0));
    }
  }

  {

    // fs.write / fs.append / file.write (legacy alias).
    // Payload: first line = path, remainder = content.
    if ((strcmp(cmd, "fs.write") == 0 || strcmp(cmd, "file.write") == 0 ||
         strcmp(cmd, "fs.append") == 0) && payload && plen > 0) {
      const char* mode = (strcmp(cmd, "fs.append") == 0) ? "a" : "w";
      const char* nl = strchr(payload, '\n');
      JsonDocument resp;
      resp["cmd"] = cmd;
      if (hasReqId) resp["req_id"] = reqId;
      const size_t kPathCap = 64;
      if (!nl) {
        resp["ok"] = false;
        resp["output"][0] = "Usage: payload = <path>\\n<content>";
      } else if ((size_t)(nl - payload) >= kPathCap) {
        // Reject rather than truncate - a clipped path writes the wrong file.
        resp["ok"] = false;
        resp["output"][0] = "Path too long";
      } else {
        char path[kPathCap];
        size_t pathLen = (size_t)(nl - payload);
        memcpy(path, payload, pathLen);
        path[pathLen] = '\0';
        const char* content = nl + 1;
        size_t contentLen = plen - pathLen - 1;

        // Broker ACL is the only external gate; reject traversal here.
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

    // fs.cat chunked: "<path> <offset> <length>" returns a JSON byte range.
    // Without offset+length, falls through to Shell::execute (line-by-line).
    if (strcmp(cmd, "fs.cat") == 0 && payload && plen > 0) {
      char pbuf[256];
      if (plen >= sizeof(pbuf)) {
        // Reject rather than truncate - a clipped "path off len" would
        // read the wrong file or byte range.
        JsonDocument resp;
        resp["cmd"] = cmd;
        if (hasReqId) resp["req_id"] = reqId;
        resp["ok"] = false;
        resp["output"][0] = "Args too long";
        char respTopic[64];
        snprintf(respTopic, sizeof(respTopic), "%s/cli/response", prefix);
        size_t bufSz = _bufferOut > 0 ? _bufferOut : 4096;
        char* rp = (char*)malloc(bufSz);
        if (rp) { serializeJson(resp, rp, bufSz); MQTTClient::publish(respTopic, rp); free(rp); }
        goto cleanup;
      }
      strncpy(pbuf, payload, min(plen, sizeof(pbuf) - 1));
      pbuf[min(plen, sizeof(pbuf) - 1)] = '\0';

      char* path = strtok(pbuf, " ");
      char* offStr = strtok(nullptr, " ");
      char* lenStr = strtok(nullptr, " ");

      if (path && offStr && lenStr) {
        size_t offset = (size_t)atol(offStr);
        size_t chunkLen = (size_t)atol(lenStr);
        // Half of output buffer; response JSON has significant overhead.
        size_t maxChunk = _bufferOut > 512 ? _bufferOut / 2 : 512;
        if (chunkLen == 0) chunkLen = maxChunk;
        if (chunkLen > maxChunk) chunkLen = maxChunk;

        JsonDocument resp;
        resp["cmd"] = cmd;
        if (hasReqId) resp["req_id"] = reqId;

        if (!Shell::pathSafe(path)) {  // same attack surface as fs.write
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
              resp["data"] = buf;  // ArduinoJson escapes on serialise
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
      // No offset/length: fall through to Shell::execute.
    }

    // cert.set: payload = "<type>\n<PEM>" (type: client_cert|client_key).
    // Stored in NVS; survives factory reset. cert.apply reconnects with mTLS.
    if (strcmp(cmd, "cert.set") == 0 && payload && plen > 0) {
      JsonDocument resp;
      resp["cmd"] = cmd;
      if (hasReqId) resp["req_id"] = reqId;

      char type[32];
      const char* pem; size_t pemLen;
      CliSplit st = cliSplitFieldValue(payload, plen, type, sizeof(type),
                                       &pem, &pemLen);
      if (st == CliSplit::NoNewline) {
        resp["ok"] = false;
        resp["output"][0] = "Usage: payload = <type>\\n<PEM>  (type: client_cert|client_key)";
      } else {
        if (st == CliSplit::FieldTooLong) {
          resp["ok"] = false;
          resp["output"][0] = "type too long";
        } else if (pemLen >= CERT_MAX_LEN) {
          resp["ok"] = false;
          resp["output"][0] = "PEM too large (>4000 B)";
        } else {
          if (strcmp(type, "client_cert") == 0) {
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
              // Zeroize before free: a lingering key in heap can surface via
              // debug commands or crash dumps. mbedtls_platform_zeroize is
              // not optimised away by the compiler.
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
      }

      char respTopic[64];
      snprintf(respTopic, sizeof(respTopic), "%s/cli/response", prefix);
      size_t bufSz = _bufferOut > 0 ? _bufferOut : 4096;
      char* rp = (char*)malloc(bufSz);
      if (rp) { serializeJson(resp, rp, bufSz); MQTTClient::publish(respTopic, rp); free(rp); }
      goto cleanup;
    }

    // secret.set: payload = "<field>\n<value>". Provisions a per-device secret
    // into NVS so config.json can stay blank. Mirrors cert.set; value zeroized.
    if (strcmp(cmd, "secret.set") == 0 && payload && plen > 0) {
      JsonDocument resp;
      resp["cmd"] = cmd;
      if (hasReqId) resp["req_id"] = reqId;

      char field[48];
      const char* value; size_t valueLen;
      CliSplit st = cliSplitFieldValue(payload, plen, field, sizeof(field),
                                       &value, &valueLen);
      if (st == CliSplit::NoNewline) {
        resp["ok"] = false;
        resp["output"][0] = "Usage: payload = <field>\\n<value>";
      } else if (st == CliSplit::FieldTooLong) {
        resp["ok"] = false;
        resp["output"][0] = "field too long";
      } else if (valueLen >= Secret::MAX_LEN) {
        resp["ok"] = false;
        resp["output"][0] = "value too long";
      } else {
        char* buf = (char*)malloc(valueLen + 1);
        if (!buf) {
          resp["ok"] = false;
          resp["output"][0] = "malloc failed";
        } else {
          memcpy(buf, value, valueLen);
          buf[valueLen] = '\0';
          bool ok = Secret::set(field, buf);
          mbedtls_platform_zeroize(buf, valueLen + 1);
          free(buf);
          resp["ok"] = ok;
          resp["output"][0] = ok ? "secret stored in NVS"
                                 : "unknown field or NVS write failed";
        }
      }

      char respTopic[64];
      snprintf(respTopic, sizeof(respTopic), "%s/cli/response", prefix);
      size_t bufSz = _bufferOut > 0 ? _bufferOut : 4096;
      char* rp = (char*)malloc(bufSz);
      if (rp) { serializeJson(resp, rp, bufSz); MQTTClient::publish(respTopic, rp); free(rp); }
      goto cleanup;
    }

    // Build "<cmd> <payload>" for Shell::execute. "{}" is a no-arg sentinel
    // (SIM7080G drops empty SMSUB URCs; platform substitutes "{}") and
    // req_id-only envelopes are also no-arg. Commands needing structured
    // JSON cannot use this path - add them as special cases above.
    char line[1024];
    bool isEmptyJson =
      payload && plen == 2 && payload[0] == '{' && payload[1] == '}';
    // {"req_id": ...} with no other keys is also a no-arg invocation -
    // the envelope is purely for correlation, not arguments. Without
    // this the envelope JSON would be passed verbatim as a literal arg
    // and Shell command parsers would see it as garbage.
    int lineN;
    if (payload && plen > 0 && !isEmptyJson && !reqIdOnlyPayload) {
      lineN = snprintf(line, sizeof(line), "%s %s", cmd, payload);
    } else {
      lineN = snprintf(line, sizeof(line), "%s", cmd);
    }
    if (lineN < 0 || (size_t)lineN >= sizeof(line)) {
      // cmd + payload exceeds the 1024-byte shell line buffer; report and
      // bail out rather than silently truncating the command string.
      JsonDocument resp;
      resp["cmd"] = cmd;
      if (hasReqId) resp["req_id"] = reqId;
      resp["ok"] = false;
      resp["output"][0] = "Command line too long for shell - use chunked variant";
      char respTopic[64];
      snprintf(respTopic, sizeof(respTopic), "%s/cli/response", prefix);
      size_t bufSz = _bufferOut > 0 ? _bufferOut : 4096;
      char* rp = (char*)malloc(bufSz);
      if (rp) { serializeJson(resp, rp, bufSz); MQTTClient::publish(respTopic, rp); free(rp); }
      goto cleanup;
    }

    // Paginate: when the next output line would overflow the publish buffer,
    // ship with more=true and start a fresh page. Final page is more=false.
    // Single-page (common case) is page 0/more=false, same shape old
    // consumers already handle.
    size_t bufSz = _bufferOut > 0 ? _bufferOut : 4096;
    // Headroom for envelope keys (cmd, req_id, ok, page, more) + NUL.
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
      // Re-read prefix: config.reload mid-command invalidates the old pool.
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
      // +8 slack covers ASCII output; pathological escape-heavy lines may
      // truncate one page rather than drop silently.
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
  return;
}

// ---------------------------------------------------------------------------

// Re-register core subscriptions (cli/#, OTA) under the current prefix.
// Called after config.reload when mqtt.topic_prefix changed. Does not
// re-register EventBus handlers or reload TLS certs.
void MQTTClient::reinitSubscriptions() {
  for (int i = 0; i < MQTT_MAX_SUBS; i++) _subs[i].active = false;
  _subCount = 0;

  JsonObject  cfg    = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  char cliTopic[64];
  snprintf(cliTopic, sizeof(cliTopic), "%s/cli/#", prefix);

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

  char availTopic[64];
  snprintf(availTopic, sizeof(availTopic), "%s/status", prefix);

  auto makeSlug = [](const char* name, char* slug, size_t sz) {
    strncpy(slug, name, sz - 1);
    slug[sz - 1] = '\0';
    for (char* p = slug; *p; p++) { if (*p == ' ') *p = '_'; *p = tolower(*p); }
  };

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
    // Drain TCP between retained publishes; CYD 1024 B buffers overflow
    // without this and spam EAGAIN. No-op but harmless on cellular path.
    if (_client.connected()) _client.loop();
    yield();
  };

  char slug[32], uid[48], stBuf[96];

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

  JsonArray channels = cfg["ads1115"]["channels"].as<JsonArray>();
  if (channels) {
    for (JsonObject ch : channels) {
      const char* cname = ch["name"] | "unknown";
      makeSlug(cname, slug, sizeof(slug));

      snprintf(uid, sizeof(uid), "%s_%s_current", devId, slug);
      snprintf(stBuf, sizeof(stBuf), "%s/sensor/current/%s", prefix, slug);
      disc("sensor", uid, cname, stBuf, "A", "current", "measurement");

      char pname[64];
      snprintf(pname, sizeof(pname), "%s Power", cname);
      snprintf(uid, sizeof(uid), "%s_%s_power", devId, slug);
      snprintf(stBuf, sizeof(stBuf), "%s/sensor/power/%s", prefix, slug);
      disc("sensor", uid, pname, stBuf, "W", "power", "measurement");
    }
  }

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

  snprintf(uid, sizeof(uid), "%s_wifi_rssi", devId);
  snprintf(stBuf, sizeof(stBuf), "%s/sensor/wifi/rssi", prefix);
  disc("sensor", uid, "WiFi RSSI", stBuf, "dBm", "signal_strength", "measurement", "diagnostic");

  snprintf(uid, sizeof(uid), "%s_wifi_ssid", devId);
  snprintf(stBuf, sizeof(stBuf), "%s/sensor/wifi/ssid", prefix);
  disc("sensor", uid, "WiFi SSID", stBuf, "", "", "", "diagnostic");

  snprintf(uid, sizeof(uid), "%s_wifi_ip", devId);
  snprintf(stBuf, sizeof(stBuf), "%s/sensor/wifi/ip", prefix);
  disc("sensor", uid, "WiFi IP", stBuf, "", "", "", "diagnostic");

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

  publishHeapStats();

  Log::info(TAG, "HA discovery published");
}

// ---------------------------------------------------------------------------

// Publish heap/PSRAM/uptime telemetry. Called on a 5 min timer and once
// from publishDiscovery(). Caches free heap in _lastHeapFree for alert
// tagging. Routes through publish() so cellular deployments get the same
// telemetry.
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

  // WiFi diagnostics only. publishDiscovery seeds them on connect; without
  // this refresh they drift stale during long stable sessions.
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

// SHA256 hex digest of a buffer.
// in: data, len, out (>= 65 bytes). out: none.
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

// SHA256 hex digest of a LittleFS file. Returns empty-string hash if missing.
// in: path, out (>= 65 bytes). out: none.
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

  // Hash the raw files, not a re-serialised Config::get(). ArduinoJson
  // compact form never matched the on-disk bytes, making drift detection
  // permanently broken. File hash = single source of truth.
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

// Most-recently-sampled free heap, or live read if sampler has not run yet.
uint32_t MQTTClient::currentFreeHeap() {
  if (_lastHeapFree > 0) return _lastHeapFree;
  return ESP.getFreeHeap();
}

// ---------------------------------------------------------------------------

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

bool MQTTClient::connected() {
  // _client is WiFi only. s_fallbackPublishing is the connected signal when
  // cellular has taken over (same pattern as the publish guards).
  return _client.connected() || s_fallbackPublishing;
}

time_t MQTTClient::lastPublishTime() {
  return _lastPublishTime;
}

// ---------------------------------------------------------------------------
// Per-device mTLS client certificate - NVS storage
// ---------------------------------------------------------------------------

// Store cert and/or key in NVS. Pass nullptr/empty to skip a half so cert
// and key can arrive in separate cert.set messages.
// in: certPEM, keyPEM (or nullptr/empty to skip). out: true on success.
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

// Load cert + key from NVS.
// in: cert, key (>= maxLen each), maxLen. out: true only if both are present.
bool MQTTClient::loadClientCert(char* cert, char* key, size_t maxLen) {
  if (!cert || !key || maxLen < CERT_MAX_LEN) return false;
  Preferences prefs;
  if (!prefs.begin(CERT_NS, true)) return false;
  size_t certLen = prefs.getString(CERT_KEY_CERT, cert, maxLen);
  size_t keyLen  = prefs.getString(CERT_KEY_KEY,  key,  maxLen);
  prefs.end();
  return certLen > 0 && keyLen > 0;
}

// Erase cert + key from NVS. Fires the cleared hook so cellular drops its
// cached cert and active session. Safe to call when absent.
// out: true (or already absent).
bool MQTTClient::clearClientCert() {
  Preferences prefs;
  if (!prefs.begin(CERT_NS, false)) return false;
  prefs.remove(CERT_KEY_CERT);
  prefs.remove(CERT_KEY_KEY);
  prefs.end();
  if (_onCertClearedHook) _onCertClearedHook();
  return true;
}

void MQTTClient::setOnClientCertCleared(std::function<void()> fn) {
  _onCertClearedHook = fn;
}

bool MQTTClient::hasClientCert() {
  Preferences prefs;
  if (!prefs.begin(CERT_NS, true)) return false;
  // isKey is correct for NVS_TYPE_STR. getBytesLength targets NVS_TYPE_BLOB
  // and logs an error when called on a string key.
  bool ok = prefs.isKey(CERT_KEY_CERT) && prefs.isKey(CERT_KEY_KEY);
  prefs.end();
  return ok;
}

// Parse stored cert PEM and extract metadata. Never touches the private key.
// in: cn, serial, notBefore, notAfter (>= maxLen each), maxLen >= 64.
// out: true if parsed, false if missing or malformed.
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

  char* sp = serial;
  char* se = serial + maxLen - 1;
  for (size_t i = 0; i < crt.serial.len && sp + 2 < se; i++) {
    sp += sprintf(sp, "%02x", crt.serial.p[i]);
  }
  *sp = '\0';

  snprintf(notBefore, maxLen, "%04d-%02d-%02dT%02d:%02d:%02dZ",
           crt.valid_from.year, crt.valid_from.mon, crt.valid_from.day,
           crt.valid_from.hour, crt.valid_from.min, crt.valid_from.sec);
  snprintf(notAfter, maxLen, "%04d-%02d-%02dT%02d:%02d:%02dZ",
           crt.valid_to.year, crt.valid_to.mon, crt.valid_to.day,
           crt.valid_to.hour, crt.valid_to.min, crt.valid_to.sec);

  mbedtls_x509_crt_free(&crt);
  return true;
}
