// thesada-fw - MQTTClient.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "MQTTClient.h"
#include "cert_policy.h"
#include "secret_heap.h"
#include "SecretJson.h"
#include <thesada_config.h>
#include "Config.h"
#include "Secret.h"
#include "cli_payload.h"
#include "cli_page_policy.h"
#include "cli_topics.h"
#include "mqtt_topic_policy.h"
#include "cli_authz_policy.h"
#include "Identity.h"
#include "mqtt_rollback_policy.h"
#include "cmd_config_policy.h"
#include "clock_floor_policy.h"
#include "temp_guard_policy.h"
#include "EventBus.h"
#include "WiFiManager.h"
#include "Log.h"
#include "Shell.h"
#include "OTAUpdate.h"
#include "ota_ca_progmem.h"
#include "TlsClient.h"
#include <mbedtls/net_sockets.h>
#include "Net.h"
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
#include <sys/time.h>
#include <esp_chip_info.h>
#include <esp_task_wdt.h>

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
static const char* CLK_KEY_FLOOR  = "clk_floor";     // uint32: last persisted
                                                     //   clock floor, epoch (s)

// Last floor value read from / written to NVS this boot. Bounds persist
// writes to one per CLOCK_FLOOR_PERSIST_INTERVAL_S.
static uint32_t s_clkFloorStored = 0;

// Buffer for the connection-critical mqtt subset (broker/port/user/password)
// serialized as JSON. Comfortably fits a hostname + creds.
static constexpr size_t LG_MAX_LEN = 384;

// Consecutive exhaustion reboots older than this age out: a one-off
// outage weeks apart never accumulates toward a halt. Only applied when
// the system clock is set.
static constexpr uint32_t RB_STALE_WINDOW_S = 6UL * 3600UL;

// Set for the duration of one inbound dispatch: did the session that
// delivered it check the broker certificate?
static bool _dispatchTlsVerified = false;

// Did the stored client cert fail to load or validate at the last connect
// attempt? Absent cert is not broken - there is nothing to recover.
static bool _storedCertBroken = false;

// Auth of the session delivering the message being dispatched right now, read
// by the CLI subscription before it hands work to the deferred ring.
static CliAuthMode _inboundAuth = CLI_AUTH_PASSWORD;

// in: whether that session presented a client cert. out: the CLI auth mode.
// Without TLS no per-device identity can exist, so the gate is a no-op.
static CliAuthMode cliAuthModeFor(bool mtls) {
#ifdef MQTT_TLS
  return mtls ? CLI_AUTH_MTLS : CLI_AUTH_PASSWORD;
#else
  (void)mtls;
  return CLI_AUTH_MTLS;
#endif
}

static const char* TAG = "MQTT";

// One session, one TLS stack: TlsClient rides on the WiFi socket or on the
// modem socket, whichever transport is live.
static WiFiClient s_wifiTcp;
enum class MqttTransport : uint8_t { NONE, WIFI, CELLULAR };
static MqttTransport s_transport      = MqttTransport::NONE;
static bool          s_cellularActive = false;

static const char* transportName(MqttTransport t) {
  return t == MqttTransport::WIFI ? "wifi" : t == MqttTransport::CELLULAR ? "cellular" : "none";
}

#ifdef MQTT_TLS
static TlsClient s_tls;
// Buffers held live across the TLS session; TlsClient stores the raw
// pointer and parses it on every handshake.
static char*     _clientCert    = nullptr;
static char*     _clientKey     = nullptr;
static bool      _mtlsActive    = false;

// Persistent broker hostname buffer. PubSubClient::setServer(const char*,
// uint16_t) stores the pointer, not the contents, so we cannot hand it
// a pointer into the ArduinoJson document - Config::load() (from
// config.reload) resets the pool and invalidates the pointer, later
// reconnects then feed garbage into WiFi.hostByName() and the DNS lookup
// fails with the previous buffer bytes as the "hostname". Copy once into
// this buffer on each read.
static char      _brokerHost[MQTT_BROKER_HOST_CAP] = {0};

// Port last handed to setServer, i.e. the one the session actually uses.
// mqtt.port in Config can already hold a value no connect has applied yet.
static uint16_t  _brokerPort = 0;

// A host past _brokerHost is refused, never dialed clipped (mqttHostCopy).
static void logBrokerRefused(const char* host, const char* action) {
  Log::kvfw(TAG, "mqtt.broker_refused reason=too_long len=%u cap=%u action=%s",
            (unsigned)strlen(host), (unsigned)sizeof(_brokerHost), action);
}

// Validate a PEM cert + key pair via mbedtls before it reaches the TLS
// client, so a bad pair is refused at the door. Also rejects
// a mismatched pair (cert A + key B): each parses fine on its own, so
// without the explicit pair check the mismatch only surfaces later as an
// opaque "TLS handshake failed".
// in: cert (PEM string), key (PEM string).
// out: true if both parse AND the key is the private half of the cert's
//      public key.
#if MBEDTLS_VERSION_MAJOR >= 3
#include <esp_random.h>
// mbedtls 3 needs an RNG for the key parse and the EC pair check (blinding);
// a NULL one fails every pair. The hardware RNG is enough here.
static int certCheckRng(void*, unsigned char* out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}
#endif

static bool validateClientCertKey(const char* cert, const char* key) {
  // Structural pre-flight before mbedtls sees attacker-influenced bytes:
  // both present, and each actually shaped like what it claims to be.
  if (!certKeyInputsUsable(cert, key)) return false;

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
                             nullptr, 0, certCheckRng, nullptr) != 0)
      break;
    if (mbedtls_pk_check_pair(&crt.pk, &pk, certCheckRng, nullptr) != 0)
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
// Heap buffer for a LittleFS /ca.crt, freed by nobody and owned here.
static char*       _caHeap    = nullptr;
// What setCACert is handed: either _caHeap or the .rodata bundle.
static const char* _caCert    = nullptr;
static size_t      _caCertLen = 0;
static bool        _tlsRefused = false;
PubSubClient MQTTClient::_client(s_tls);
#else
PubSubClient MQTTClient::_client(s_wifiTcp);
#endif
// True only after setCACert. allow_insecure encrypts without checking the
// broker, and that session must not accept a config push.
static bool _tlsVerified = false;

// Calls into PubSubClient under way on the loop task. A modem wait inside one
// can run a console net.http, whose tick() must not re-enter the client.
static int s_clientCalls = 0;
struct ClientCall {
  ClientCall()  { ++s_clientCalls; }
  ~ClientCall() { --s_clientCalls; }
};

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

MQTTSubTable     MQTTClient::_subs;
uint32_t         MQTTClient::_lastSuccessMs    = 0;
uint32_t         MQTTClient::_connectedSinceMs = 0;
uint32_t         MQTTClient::_lastHeapPublishMs = 0;
uint32_t         MQTTClient::_lastHeapFree      = 0;
uint32_t         MQTTClient::_lowHeapSinceMs    = 0;
bool             MQTTClient::_reinitPending     = false;
bool             MQTTClient::_certRebootPending = false;
uint32_t         MQTTClient::_certRebootAtMs    = 0;
const char*      MQTTClient::_certRebootReason  = "";

char             MQTTClient::_rxRing[MQTTClient::RX_RING_SIZE][96] = {};
uint32_t         MQTTClient::_rxRingTs[MQTTClient::RX_RING_SIZE]    = {};
uint8_t          MQTTClient::_rxRingHead  = 0;
uint8_t          MQTTClient::_rxRingCount = 0;

std::vector<String> MQTTClient::_retainedTopics;
bool     MQTTClient::_manifestPublished    = false;
bool     MQTTClient::_manifestDirty        = false;
uint32_t MQTTClient::_manifestDirtySinceMs = 0;
bool     MQTTClient::_retainedPublishedThisSession = false;

// Which transport the next session should use: WiFi whenever it is up,
// the modem socket while CellularModule has the cellular link active.
static MqttTransport transportWanted() {
  if (WiFiManager::connected()) return MqttTransport::WIFI;
  const Net::CellularProvider* cell = Net::cellular();
  if (s_cellularActive && cell && cell->tcp && cell->tcp(0)) return MqttTransport::CELLULAR;
  return MqttTransport::NONE;
}

// Lower byte stream for a transport. in: transport. out: client or nullptr.
static Client* transportClient(MqttTransport t) {
  if (t == MqttTransport::WIFI) return &s_wifiTcp;
  if (t == MqttTransport::CELLULAR) {
    const Net::CellularProvider* cell = Net::cellular();
    return (cell && cell->tcp) ? cell->tcp(0) : nullptr;
  }
  return nullptr;
}

// Drop the live session and its socket, whatever carries it.
static void closeSession() {
#ifdef MQTT_TLS
  s_tls.stop();
#else
  if (Client* c = transportClient(s_transport)) c->stop();
#endif
}

void MQTTClient::setCellularTransport(bool active) {
  if (active == s_cellularActive) return;
  s_cellularActive = active;
  if (active && _queueCount > 0) {
    // Samples buffered during the WiFi outage are stale, and replaying
    // them all at once would crowd the first seconds on the modem link.
    // Sensors are level-triggered and refire on their next cycle.
    Log::kvf(TAG, "mqtt.queue_dropped reason=cellular_handoff count=%d", (int)_queueCount);
    for (uint8_t i = 0; i < MQTT_QUEUE_SIZE; ++i) _queue[i].valid = false;
    _queueHead  = 0;
    _queueTail  = 0;
    _queueCount = 0;
  }
  Log::kvf(TAG, "mqtt.cellular_transport active=%d", active ? 1 : 0);
  // Reconnect on the right transport at the next loop, not after backoff.
  _retryInterval = RETRY_MIN_MS;
  _lastAttempt   = millis() - RETRY_MIN_MS;
}

void MQTTClient::dropSession() {
  ClientCall c;
  _client.disconnect();
  closeSession();
}

const char* MQTTClient::transportName() {
  return _client.connected() ? ::transportName(s_transport) : "none";
}

// ---------------------------------------------------------------------------

// Shared inbound CLI dispatch for both subscribe registrations (begin and
// reinitSubscriptions) - one copy or the two drift. Defers to the Shell ring:
// executing inside the PubSubClient callback blocks keepalive and causes
// disconnects on slow operations (LittleFS writes, config reload, etc).
// in: raw topic + payload from the broker callback. out: none.
static void cliInboundHandler(const char* topic, const char* payload) {
  JsonObject  cfg    = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  char cliPrefix[CLI_TOPIC_CAP];
  // A truncated prefix would match short and slice the wrong command out.
  if (!cliInputPrefix(cliPrefix, sizeof(cliPrefix), prefix)) return;
  // The subscription is skipped in this mode; the guard also covers a
  // retained message delivered before a reinit drops the topic.
  if (!shellModeMqttAllowed(Shell::mode())) return;
  size_t prefixLen = strlen(cliPrefix);
  if (strncmp(topic, cliPrefix, prefixLen) != 0) return;
  const char* cmd = topic + prefixLen;
  if (strlen(cmd) == 0 || strcmp(cmd, "response") == 0) return;

  // Heap copies the lambda owns past this callback. The payload copy is wiped
  // when the slot's DeferredFn is reset on drain, or dropped unrun.
  std::string cmdCopy(cmd);
  SecretString payloadCopy(payload, payload ? strlen(payload) : 0);

  // Frozen here, not read at drain time: the transport can fail over
  // between the two and the gate must judge the session that spoke.
  CliAuthMode auth = _inboundAuth;
  bool ok = Shell::enqueueDeferred(
    [cmdCopy = std::move(cmdCopy), payloadCopy = std::move(payloadCopy), auth]() {
      MQTTClient::runCli(cmdCopy.c_str(),
                         payloadCopy.empty() ? nullptr : payloadCopy.c_str(),
                         payloadCopy.size(), auth);
    });
  if (!ok) Log::warn("MQTT", "mqtt.cli_dropped reason=busy");
}

// Staged cmd/config body. Copied out of the client callback, applied on the
// next loop tick so LittleFS and Config::replace never run inside PubSubClient.
static char _cmdConfigBuf[CMD_CONFIG_MAX];
static bool _cmdConfigPending = false;

// Copy a verified-TLS JSON object into the staging buffer.
// in: topic (unused), payload. out: none.
static void cmdConfigInbound(const char* topic, const char* payload) {
  (void)topic;
  size_t n = payload ? strlen(payload) : 0;
  switch (cmdConfigVerdict(_dispatchTlsVerified, payload, n)) {
    case CMD_CONFIG_OK:
      break;
    case CMD_CONFIG_REFUSE_TLS:
      Log::warn(TAG, "mqtt.cmd_config_refused reason=tls");
      return;
    case CMD_CONFIG_REFUSE_BROKER:
      Log::warn(TAG, "mqtt.cmd_config_refused reason=broker");
      return;
    case CMD_CONFIG_REFUSE_SHAPE:
      Log::warn(TAG, "mqtt.cmd_config_refused reason=shape");
      return;
  }
  if (_cmdConfigPending) {
    Log::warn(TAG, "mqtt.cmd_config_refused reason=busy");
    return;
  }
  memcpy(_cmdConfigBuf, payload, n);
  _cmdConfigBuf[n] = '\0';
  _cmdConfigPending = true;
}

// Subscribe <prefix>/cmd/config. Independent of shell.mode, same idea as
// the OTA command topic: a headless device can still be reconfigured.
// in: none (reads topic_prefix). out: none.
static void mqttSubscribeCmdConfig() {
  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  // Sized to the prefix cap: the gate is "prefix plus /cmd/config fits there".
  // The subscription table is MQTT_TOPIC_CAP, which is wider, so it can store it.
  char topic[Config::TOPIC_PREFIX_CAP];
  if (!cliTopicJoin(topic, sizeof(topic), prefix, "/cmd/config")) {
    Log::kvf(TAG, "mqtt.cmd_config_topic_truncated prefix=%s", prefix);
    return;
  }
  MQTTClient::subscribe(topic, cmdConfigInbound);
}

static size_t mqttCriticalJson(char* out, size_t maxLen);

// Apply a staged document. Refuses a blob that drops mqtt.broker so a push
// cannot remove the only way back. Broker, port, user, and password changes
// reconnect without rebuilding subscriptions. topic_prefix is written to
// disk and kept at the boot value until restart.
// in: JSON object text. out: none.
static void mqttApplyCmdConfig(const char* json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) || !doc.is<JsonObject>()) {
    Log::warn(TAG, "mqtt.cmd_config_refused reason=json");
    return;
  }
  const char* broker = doc["mqtt"]["broker"] | "";
  if (!broker[0]) {
    Log::warn(TAG, "mqtt.cmd_config_refused reason=broker");
    return;
  }
  if (!mqttHostFits(broker, MQTT_BROKER_HOST_CAP)) {
    Log::kvfw(TAG, "mqtt.cmd_config_refused reason=broker_too_long len=%u cap=%u",
              (unsigned)strlen(broker), (unsigned)MQTT_BROKER_HOST_CAP);
    return;
  }
  char prefixLive[Config::TOPIC_PREFIX_CAP];
  bool prefixFits = false;
  {
    const char* prefix = Config::get()["mqtt"]["topic_prefix"] | "thesada/node";
    size_t n = strlen(prefix);
    if (n < sizeof(prefixLive)) {
      memcpy(prefixLive, prefix, n + 1);
      prefixFits = true;
    }
  }
  JsonVariantConst incomingVar = doc["mqtt"]["topic_prefix"];
  if (!incomingVar.isUnbound() && !incomingVar.is<const char*>()) {
    Log::warn(TAG, "mqtt.cmd_config_refused reason=prefix_type");
    return;
  }
  const char* incoming = incomingVar.is<const char*>()
                             ? incomingVar.as<const char*>()
                             : "thesada/node";
  if (!incoming) {
    Log::warn(TAG, "mqtt.cmd_config_refused reason=prefix_type");
    return;
  }
  // The subscribe buffer is TOPIC_PREFIX_CAP, and "/cmd/config" has to fit
  // in it with the prefix. A shorter check would save a prefix that can
  // never be subscribed after restart.
  if (!prefixFits ||
      strlen(incoming) + strlen("/cmd/config") >= Config::TOPIC_PREFIX_CAP) {
    Log::warn(TAG, "mqtt.cmd_config_refused reason=prefix_length");
    return;
  }
  char before[LG_MAX_LEN];
  size_t nb = mqttCriticalJson(before, sizeof(before));
  if (!Config::replace(json)) {
    Log::warn(TAG, "mqtt.cmd_config_refused reason=persist");
    return;
  }
  const char* prefixNow = Config::get()["mqtt"]["topic_prefix"] | "thesada/node";
  if (strcmp(prefixLive, prefixNow) != 0) {
    if (!Config::holdTopicPrefix(prefixLive)) {
      Log::error(TAG, "mqtt.cmd_config_prefix_unheld");
    } else {
      Log::info(TAG, "mqtt.cmd_config_prefix_deferred");
    }
  }
  char after[LG_MAX_LEN];
  size_t na = mqttCriticalJson(after, sizeof(after));
  if (nb == 0 || na == 0) {
    Log::info(TAG, "mqtt.cmd_config_reinit reason=snapshot");
    MQTTClient::reconnectWithCurrentConfig();
    return;
  }
  if (strcmp(before, after) != 0) {
    Log::info(TAG, "mqtt.cmd_config_reinit reason=connection_keys");
    MQTTClient::reconnectWithCurrentConfig();
    return;
  }
  Log::info(TAG, "mqtt.cmd_config_applied");
}

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
    Log::error(TAG, "mqtt.config_missing key=broker");
    return;
  }

  _bufferIn  = cfg["mqtt"]["buffer_in"]  | 4096;
  _bufferOut = cfg["mqtt"]["buffer_out"] | 4096;
  if (_bufferIn  < 512)  _bufferIn  = 512;
  if (_bufferIn  > 8192) _bufferIn  = 8192;
  if (_bufferOut < 512)  _bufferOut = 512;
  if (_bufferOut > 8192) _bufferOut = 8192;

  // Copy into persistent buffer - see _brokerHost comment above. A refused
  // host leaves it empty: connects fail and the budget rolls back to last-good.
  if (mqttHostCopy(_brokerHost, sizeof(_brokerHost), host)) {
    _brokerPort = port;
    _client.setServer(_brokerHost, port);
  } else {
    logBrokerRefused(host, "no_broker");
  }
  _client.setKeepAlive(60);
  _client.setSocketTimeout(5);  // CONNACK and read waits, s (default 15)
  _client.setBufferSize(_bufferIn);
  _client.setCallback(onMessage);

#ifdef MQTT_TLS
  _tlsVerified = false;
  // Boot clock floor: lift an epoch clock to a sane lower bound of real time
  // before the first TLS handshake, so certificate validity checks pass from
  // the very first connect and no insecure window exists. Floor = newer of
  // the persisted floor and the firmware build stamp; the clock is never
  // moved backwards, so an already-synced clock is untouched.
  {
    Preferences prefs;
    if (prefs.begin(BOOT_NS, true)) {
      s_clkFloorStored = prefs.getUInt(CLK_KEY_FLOOR, 0);
      prefs.end();
    }
    const uint32_t floorEpoch =
        clockFloorTarget(s_clkFloorStored, clockFloorBuildEpoch(__DATE__, __TIME__));
    if (clockFloorShouldApply(time(nullptr), floorEpoch)) {
      struct timeval tv = { (time_t)floorEpoch, 0 };
      settimeofday(&tv, nullptr);
      Log::kvf(TAG, "mqtt.clock_floor_applied epoch=%lu ntp=corrects_forward",
               (unsigned long)floorEpoch);
    }
  }

  // Dial bounds: WiFi TCP connect 10 s, handshake 20 s (a modem link needs
  // several seconds). TlsClient feeds the task watchdog while it waits.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  s_wifiTcp.setConnectionTimeout(10000);  // ms
#else
  s_wifiTcp.setTimeout(10);  // seconds
#endif
  s_tls.setHandshakeTimeout(20000);

  // Load CA cert from LittleFS, else point straight at the baked bundle.
  // The bundle needs no copy: it is .rodata with static lifetime and
  // setCACert keeps the pointer, so this path cannot fail on low heap.
  if (LittleFS.exists("/ca.crt")) {
    File cf = LittleFS.open("/ca.crt", "r");
    if (cf) {
      size_t sz = cf.size();
      if (sz > 0) {
#if defined(BOARD_HAS_PSRAM)
        _caHeap = (char*)heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM);
        const char* heapTag = "PSRAM";
#else
        _caHeap = (char*)malloc(sz + 1);
        const char* heapTag = "heap";
#endif
        if (_caHeap) {
          size_t readBytes = cf.readBytes(_caHeap, sz);
          _caHeap[readBytes] = '\0';
          _caCert    = _caHeap;
          _caCertLen = readBytes;
          Log::kvf(TAG, "mqtt.ca_loaded path=/ca.crt bytes=%u heap=%s",
                   (unsigned)readBytes, heapTag);
        } else {
          Log::error(TAG, "mqtt.ca_alloc_failed");
        }
      }
      cf.close();
    }
  }
  // Same roots as OTA. Reached when LittleFS has no /ca.crt, and also when
  // reading it failed - a low-heap boot lands on verified TLS, not on none.
  if (!_caCert || _caCertLen == 0) {
    size_t pmLen = strlen_P(OTA_CA_PROGMEM);
    if (pmLen > 0) {
      _caCert    = OTA_CA_PROGMEM;
      _caCertLen = pmLen;
      Log::kvfw(TAG, "mqtt.ca_fallback source=progmem bytes=%u", (unsigned)pmLen);
    }
  }
  if (!_caCert || _caCertLen == 0) {
    // Only an empty baked bundle reaches here, which is a build fault, so
    // it is opt-in rather than automatic. Mirrors ota.allow_insecure.
    bool allowInsecure = cfg["mqtt"]["allow_insecure"] | false;
    if (allowInsecure) {
      Log::kvfw(TAG, "mqtt.tls_insecure reason=no_ca allow_insecure=set");
      s_tls.setInsecure();
      _tlsVerified = false;
    } else {
      Log::error(TAG, "mqtt.tls_refused reason=no_ca allow_insecure=unset");
      _tlsRefused = true;
      return;
    }
  } else {
    s_tls.setCACert(_caCert);
    _tlsVerified = true;
  }
#endif

  // Empty table, then put OTA back: main.cpp registers it before MQTT for
  // heap reasons, so without this ota.cmd_topic is dead for the whole boot.
  _subs.reset();
  OTAUpdate::registerCommandTopic();

  EventBus::subscribe("alert", [](JsonObject data) {
    JsonObject  cfg    = Config::get();
    const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
    char topic[MQTT_TOPIC_CAP];
    if (!MQTTClient::formatTopic(topic, sizeof(topic), "%s/alert", prefix)) return;
    char payload[256];
    serializeJson(data, payload, sizeof(payload));
    MQTTClient::publish(topic, payload);
  });

  {
    JsonObject  cfg    = Config::get();
    const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
    char cliTopic[CLI_TOPIC_CAP];
    if (!cliInputSubscription(cliTopic, sizeof(cliTopic), prefix)) {
      Log::kvf("MQTT", "mqtt.cli_topic_truncated prefix=%s", prefix);
      mqttSubscribeCmdConfig();
      return;
    }

    // The generic shell-line path goes through Shell::enqueue;
    // binary-payload commands (fs.write, fs.cat chunked, cert.set) and
    // the response-shape contract (one cli/response message per command,
    // JSON array of output lines) need their own handler, so inbound CLI
    // goes through cliInboundHandler onto the same ring, same
    // backpressure, single drain path.
    if (shellModeMqttAllowed(Shell::mode())) {
      MQTTClient::subscribe(cliTopic, cliInboundHandler);
    } else {
      Log::kvf("MQTT", "mqtt.cli_disabled reason=shell_mode");
    }
  }

  mqttSubscribeCmdConfig();
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
  if (count > 0 && now > (time_t)CLOCK_FLOOR_SANE_EPOCH && epoch > 0 &&
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
  prefs.putUInt(RB_KEY_EPOCH, (now > (time_t)CLOCK_FLOOR_SANE_EPOCH) ? (uint32_t)now : 0);
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

// Thin wrapper over the pure, host-tested predicate (mqtt_rollback_policy.h).
bool MQTTClient::rollbackDecision(const char* lg, bool haveLg,
                                  const char* rbCfg, const char* cur) {
  return mqttRollbackShould(lg, haveLg, rbCfg, cur);
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
  JsonObject m = Config::get()["mqtt"];
  if (!mqttSnapshotProven(m["broker"] | "", m["port"] | 8883, _brokerHost, _brokerPort)) {
    Log::kvfw(TAG, "mqtt.config_commit_skipped reason=broker_not_live");
    return;
  }
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
  // Cleared before the early returns, not after: assigning it further down
  // left the previous session's verdict standing on every path that bails out.
  _mtlsActive = false;

  // begin() refused to configure TLS. Connecting anyway would hand the CLI
  // an unverified channel, which is the thing the refusal exists to stop.
#ifdef MQTT_TLS
  if (_tlsRefused) return;
#endif
  const MqttTransport transport = transportWanted();
  if (transport == MqttTransport::NONE) return;

  JsonObject  cfg      = Config::get();
  const char* clientId = Identity::nodeName();
  const char* user     = cfg["mqtt"]["user"]      | "";
  char        passwordBuf[Secret::MAX_LEN];
  const char* password = Secret::resolve("mqtt_password", cfg["mqtt"]["password"] | "",
                                         passwordBuf, sizeof(passwordBuf));
  const char* prefix   = cfg["mqtt"]["topic_prefix"] | "thesada/node";

  // A will topic that does not fit connects without a will, never a clipped one.
  char availTopic[MQTT_TOPIC_CAP];
  const char* willTopic =
      formatTopic(availTopic, sizeof(availTopic), "%s/status", prefix) ? availTopic : nullptr;

  Log::kvf(TAG, "mqtt.connect_start client_id=%s transport=%s", clientId, ::transportName(transport));

  closeSession();
  _client.connected();  // sees the stop(), so a stale CONNECTED state clears

#ifdef MQTT_TLS
  // No pre-NTP insecure fallback here: the boot clock floor in begin() keeps
  // the clock at a sane lower bound of real time, so cert validation works
  // from the first connect. A cert the floored clock rejects (e.g. NotBefore
  // ahead of a stale floor) fails closed and retries until NTP corrects.

  // Load client cert from NVS into module-level buffers. TlsClient keeps
  // the raw pointer and parses it on every handshake, so the buffers stay
  // live. Validate via mbedtls first so a bad pair never reaches it.
  if (hasClientCert()) {
    if (!_clientCert) _clientCert = (char*)secretAlloc(CERT_MAX_LEN);
    if (!_clientKey)  _clientKey  = (char*)secretAlloc(CERT_MAX_LEN);
    if (!_clientCert || !_clientKey) {
      // Heap pressure says nothing about the cert: keep the last broken
      // verdict so a full heap cannot open cert.clear to a password session.
      secretFree(_clientCert); _clientCert = nullptr;
      secretFree(_clientKey);  _clientKey  = nullptr;
      Log::warn(TAG, "mqtt.mtls_load_oom fallback=password");
    } else if (loadClientCert(_clientCert, _clientKey, CERT_MAX_LEN)) {
      if (validateClientCertKey(_clientCert, _clientKey)) {
        _storedCertBroken = false;
        // A usable cert is not enough. mTLS follows the listener this session
        // actually dials, so a session on any other port stays password.
        if (_brokerPort == MQTT_MTLS_PORT) {
          _mtlsActive = true;
          Log::info(TAG, "mqtt.mtls_cert_loaded source=nvs");
        } else {
          Log::kvfw(TAG, "mqtt.mtls_port_mismatch port=%u mtls_port=%u fallback=password",
                    (unsigned)_brokerPort, (unsigned)MQTT_MTLS_PORT);
        }
      } else {
        _storedCertBroken = true;
        Log::kvfw(TAG, "mqtt.mtls_cert_invalid fallback=password");
      }
    } else {
      // NVS rows exist but will not load: that is a broken pair. Free the
      // buffers - a stranded 4 KB pair starves the next connect attempt.
      secretFree(_clientCert); _clientCert = nullptr;
      secretFree(_clientKey);  _clientKey  = nullptr;
      _storedCertBroken = true;
      Log::warn(TAG, "mqtt.mtls_load_failed fallback=password");
    }
  } else {
    // Nothing stored cannot be broken, so cert.clear stays mTLS-only here.
    _storedCertBroken = false;
  }
  s_tls.setCertificate(_mtlsActive ? _clientCert : nullptr);
  s_tls.setPrivateKey(_mtlsActive ? _clientKey : nullptr);
  s_tls.setTransport(transportClient(transport));
#endif

  // The modem link idles out NAT state on some carriers within a minute, so
  // its keepalive is short, but longer than one socket open on the other
  // slot holds the AT bus (20 s). A modem round trip is slower too.
  const bool cellular = transport == MqttTransport::CELLULAR;
  _client.setKeepAlive(cellular ? 30 : 60);
  _client.setSocketTimeout(cellular ? 10 : 5);  // CONNACK and read waits, s

  // WiFi: core 3 DNS has no cap and hostByName returns the lwIP error, not 0,
  // on failure, so resolve and feed first, then dial the address. The modem
  // socket resolves the name itself (CDNSGIP). TLS verifies and sends SNI for
  // the configured name either way, and PubSubClient reuses the socket.
  bool resolved = true;
  bool dialed   = false;
  if (cellular) {
#ifdef MQTT_TLS
    dialed = s_tls.connect(_brokerHost, _brokerPort) == 1;
    // A failed DNS or socket open is no TLS failure: keep it off the heap budget.
    if (!dialed && s_tls.lastError() == MBEDTLS_ERR_NET_CONNECT_FAILED) resolved = false;
#else
    dialed = transportClient(transport)->connect(_brokerHost, _brokerPort) == 1;
#endif
  } else {
    IPAddress brokerIp;
    resolved = WiFi.hostByName(_brokerHost, brokerIp) == 1;
    esp_task_wdt_reset();
    if (resolved) {
#ifdef MQTT_TLS
      dialed = s_tls.connect(brokerIp, _brokerPort, _brokerHost) == 1;
#else
      dialed = s_wifiTcp.connect(brokerIp, _brokerPort) == 1;
#endif
    }
  }
  esp_task_wdt_reset();
  s_transport = transport;

  // Broker uses CN as username (use_identity_as_username) when mTLS is
  // active - sending user/pass alongside is redundant and confusing.
  bool ok = false;
  if (dialed && _mtlsActive) {
    ok = _client.connect(clientId, nullptr, nullptr, willTopic, 0, true, "offline");
  } else if (dialed) {
    ok = (strlen(user) > 0)
      ? _client.connect(clientId, user, password, willTopic, 0, true, "offline")
      : _client.connect(clientId, willTopic, 0, true, "offline");
  }

  if (ok) {
    esp_task_wdt_reset();  // the resubscribe and retained burst gets its own window
    OTAUpdate::noteMqttUp();
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
    Log::kvf(TAG, "mqtt.state_change from=disconnected to=connected broker=%s transport=%s",
             _brokerHost, ::transportName(transport));

    // TCP keepalive catches dead connections faster than MQTT-level keepalive.
    int fd = cellular ? -1 : s_wifiTcp.fd();
    if (fd >= 0) {
      int keepAlive    = 1;
      int keepIdle     = 30;   // seconds before first probe
      int keepInterval = 10;   // seconds between probes
      int keepCount    = 3;    // failed probes before declaring dead
      setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE, &keepAlive,    sizeof(keepAlive));
      setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle,     sizeof(keepIdle));
      setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL,&keepInterval, sizeof(keepInterval));
      setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,  &keepCount,    sizeof(keepCount));
      Log::info(TAG, "mqtt.tcp_keepalive_enabled idle_s=30 interval_s=10 count=3");
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
    // A failed resolve or dial never reached PubSubClient, so its state() is stale.
    const int rc = dialed ? _client.state() : MQTT_CONNECT_FAILED;
    if (dialed) closeSession();
    Log::kvfw(TAG, "mqtt.connect_failed rc=%d retry_ms=%lu",
              rc, (unsigned long)_retryInterval);
    _retryCount++;
    _retryInterval = min(_retryInterval * 2, (uint32_t)RETRY_MAX_MS);

    // mbedtls needs ~30 KB contiguous; a fragmented heap kills the TLS
    // handshake before the broker is ever reached, which reports
    // MQTT_CONNECT_FAILED. rc >= 1 means the broker answered (bad creds,
    // unavailable, ...) - heap was fine, a reboot fixes nothing. And on
    // heap-constrained boards MaxAllocHeap < 40 KB is the steady state, so
    // the heap check alone must not trigger. Consume the same NVS reboot
    // budget as the exhaustion path below - unbudgeted, a plain broker
    // outage on a CYD rebooted the device every ~15 s forever.
    if (mqttDefragRebootDue(_retryCount, resolved, rc, ESP.getMaxAllocHeap(),
                            _rebootHalted)) {
      uint8_t maxReboots = Config::get()["mqtt"]["max_exhaust_reboots"] | 3;
      uint8_t count      = mqttRebootCount();
      if (count < maxReboots) {
        Log::kvfe(TAG, "mqtt.defrag_reboot reason=tls_low_heap fails=3 reboot=%u max=%u",
                  (unsigned)(count + 1), (unsigned)maxReboots);
        mqttRebootCountBump(count + 1);
        delay(1000);
        OTAUpdate::brokerRestart();
      }
      // Budget spent: fall through to backoff; the exhaustion path below
      // owns the halt messaging.
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
        Log::kvfe(TAG, "mqtt.recovery_reboot fails=%u reboot=%u max=%u",
                  (unsigned)rebootAfter, (unsigned)(count + 1),
                  (unsigned)maxReboots);
        // Record the config that is failing so the boot-time rollback restores
        // exactly this one, not an unrelated later edit.
        char failing[LG_MAX_LEN];
        if (mqttCriticalJson(failing, sizeof(failing)) > 0) mqttWriteRbCfg(failing);
        mqttRebootCountBump(count + 1);
        delay(1000);
        OTAUpdate::brokerRestart();
      } else {
        // Budget spent. Keep retrying, never reboot again.
        _rebootHalted = true;
        Log::kvfe(TAG, "mqtt.reboot_budget_spent fails=%u reboots=%u action=retry_no_reboot hint=check_broker_config",
                  (unsigned)rebootAfter, (unsigned)maxReboots);
      }
    }
  }
}

// ---------------------------------------------------------------------------

// PubSubClient::loop() for the keepalive, between module inits and in a
// blocking shell wait. Inside another call into the client it does nothing.
void MQTTClient::tick() {
  if (s_clientCalls > 0) return;
  ClientCall c;
  if (_client.connected()) {
    _client.loop();
  }
}

// ---------------------------------------------------------------------------

void MQTTClient::loop() {
  ClientCall call;
  connected();  // refreshes _connCache once per tick
  if (_cmdConfigPending) {
    _cmdConfigPending = false;
    mqttApplyCmdConfig(_cmdConfigBuf);
  }
#ifdef MQTT_TLS
  // Persist the clock floor at most daily (see clock_floor_policy.h). The
  // clock only advances at real-time rate, so the stored value is always a
  // valid lower bound of real time for the next boot - NTP or not.
  {
    static uint32_t lastFloorCheckMs = 0;
    if (millis() - lastFloorCheckMs > 60000UL) {
      lastFloorCheckMs = millis();
      time_t nowT = time(nullptr);
      if (clockFloorShouldPersist(nowT, s_clkFloorStored)) {
        Preferences prefs;
        if (prefs.begin(BOOT_NS, false)) {
          prefs.putUInt(CLK_KEY_FLOOR, (uint32_t)nowT);
          prefs.end();
          s_clkFloorStored = (uint32_t)nowT;
          Log::info(TAG, "clock_floor.persisted");
        }
      }
    }
  }
#endif

  // Reboot if heap stays below floor for HEAP_REBOOT_HOLD_MS: mbedtls OOM
  // mid-handshake wedges the connection with no clean recovery; a fresh
  // boot defragments. Set HEAP_REBOOT_FLOOR_BYTES=0 to disable.
  if (HEAP_REBOOT_FLOOR_BYTES > 0) {
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t now = millis();
    if (freeHeap < HEAP_REBOOT_FLOOR_BYTES) {
      if (_lowHeapSinceMs == 0) {
        _lowHeapSinceMs = now;
        Log::kvfw(TAG, "mqtt.heap_watchdog_armed free_b=%lu floor_b=%d",
                  (unsigned long)freeHeap, (int)HEAP_REBOOT_FLOOR_BYTES);
      } else if ((now - _lowHeapSinceMs) >= (uint32_t)HEAP_REBOOT_HOLD_MS) {
        Log::kvfe(TAG, "mqtt.heap_watchdog_reboot floor_b=%d hold_ms=%d",
                  (int)HEAP_REBOOT_FLOOR_BYTES, (int)HEAP_REBOOT_HOLD_MS);
        delay(100);
        // Not plannedRestart: a heap that stays low is a fault, so it counts as a crash.
        esp_restart();
      }
    } else if (_lowHeapSinceMs != 0) {
      Log::info(TAG, "mqtt.heap_watchdog_disarmed reason=heap_recovered");
      _lowHeapSinceMs = 0;
    }
  }


  {
    uint32_t hpNow = millis();
    if (_client.connected() &&
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

  // Deferred reconnect after reinitSubscriptions(): clean stack gives
  // mbedtls enough room (~10 KB) for the TLS handshake.
  if (_reinitPending) {
    _reinitPending = false;
    JsonObject  cfgR = Config::get();
    const char* host = cfgR["mqtt"]["broker"] | "";
    uint16_t    port = cfgR["mqtt"]["port"]   | 8883;
    // Refresh persistent buffer, as the re-parsed pool invalidates the prior
    // pointer. A host that does not fit keeps the previous broker and port.
    if (mqttHostCopy(_brokerHost, sizeof(_brokerHost), host)) {
      _brokerPort = port;
    } else {
      logBrokerRefused(host, "keep_previous");
    }
    _client.setServer(_brokerHost, _brokerPort);
    _retryInterval = RETRY_MIN_MS;
    connect();
  }

  // WiFi came back under a cellular session, or the cellular link was
  // withdrawn: move the session to the transport that should carry it.
  if (_client.connected() && transportWanted() != s_transport) {
    Log::kvf(TAG, "mqtt.transport_switch from=%s to=%s",
             ::transportName(s_transport), ::transportName(transportWanted()));
    _client.disconnect();
    closeSession();
    _lastSuccessMs    = 0;
    _connectedSinceMs = 0;
    _retryInterval    = RETRY_MIN_MS;
    _lastAttempt      = millis() - RETRY_MIN_MS;
  }

  if (_client.connected()) {
    uint32_t now = millis();
    if (_lastSuccessMs > 0 && (now - _lastSuccessMs > WATCHDOG_MS)) {
      uint32_t uptime = (now - _connectedSinceMs) / 1000;
      Log::kvfw(TAG, "mqtt.state_change from=connected to=disconnected reason=watchdog_no_activity idle_s=%lu uptime_s=%lu",
                (unsigned long)(WATCHDOG_MS / 1000), (unsigned long)uptime);
      _client.disconnect();
      closeSession();
      _lastSuccessMs = 0;
      _connectedSinceMs = 0;
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
    Log::kvfw(TAG, "mqtt.state_change from=connected to=disconnected reason=connection_lost uptime_s=%lu",
              (unsigned long)uptime);
    _connectedSinceMs = 0;
  }

  if (transportWanted() == MqttTransport::NONE) return;

  uint32_t now = millis();
  if (now - _lastAttempt >= _retryInterval) {
    _lastAttempt = now;
    connect();
  }
}

// ---------------------------------------------------------------------------

void MQTTClient::publish(const char* topic, const char* payload) {
  // An empty topic is a protocol error the broker answers by disconnecting.
  if (!topic || !*topic) {
    Log::warn(TAG, "mqtt.publish_refused reason=empty_topic");
    return;
  }
  ClientCall c;
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

// Publish retained. Records the topic in the manifest so a device-delete
// sweep can clear it. Dropped while no session is up: the retained set is
// republished in full on every connect.
void MQTTClient::publishRetained(const char* topic, const char* payload) {
  if (!topic || !*topic) {
    Log::warn(TAG, "mqtt.publish_refused reason=empty_topic");
    return;
  }
  ClientCall c;
  if (!_client.connected()) return;
  _client.publish(topic, (const uint8_t*)payload, strlen(payload), true);
  recordRetainedTopic(topic);
}

bool MQTTClient::publishNow(const char* topic, const char* payload) {
  if (!topic || !*topic) {
    Log::warn(TAG, "mqtt.publish_refused reason=empty_topic");
    return false;
  }
  if (!_client.connected()) {
    enqueue(topic, payload);
    return false;
  }
  if (!_client.publish(topic, payload)) {
    Log::kvfw(TAG, "mqtt.publish_failed payload_b=%u topic=%s",
              (unsigned)strlen(payload), topic);
    return false;
  }
  _lastPublishTime = time(nullptr);
  return true;
}

bool MQTTClient::formatTopic(char* out, size_t cap, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  bool fit = mqttTopicVfmt(out, cap, fmt, ap);
  va_end(ap);
  if (!fit) {
    Log::kvfw(TAG, "mqtt.topic_refused reason=too_long cap=%u fmt=%s",
              (unsigned)cap, fmt ? fmt : "");
  }
  return fit;
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
  if (!_client.connected()) return;

  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";

  char topic[MQTT_TOPIC_CAP];
  if (!formatTopic(topic, sizeof(topic), "%s/info/retained_topics", prefix)) return;

  recordRetainedTopic(topic);

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto& t : _retainedTopics) arr.add(t);

  String payload;
  serializeJson(doc, payload);

  if (!_client.publish(topic, (const uint8_t*)payload.c_str(), payload.length(), true)) return;
  _manifestPublished    = true;
  _manifestDirty        = false;
  _manifestDirtySinceMs = 0;
  Log::kvf(TAG, "mqtt.manifest_published entries=%u", (unsigned)_retainedTopics.size());
}

// ---------------------------------------------------------------------------

// Republish retained state: availability, discovery, /info, manifest.
// connect() runs it with force on every new session; the session flag
// stops a non-forced caller from repeating it inside one session.
// in: force - bypass the session flag.
// out: none.
void MQTTClient::publishRetainedSet(bool force) {
  if (!_client.connected()) return;
  if (!force && _retainedPublishedThisSession) return;

  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";

  char availTopic[MQTT_TOPIC_CAP];
  if (formatTopic(availTopic, sizeof(availTopic), "%s/status", prefix)) {
    publishRetained(availTopic, "online");
  }

  publishDiscovery();
  publishDeviceInfo();
  publishRetainedManifest();

  _retainedPublishedThisSession = true;
}

// ---------------------------------------------------------------------------

void MQTTClient::subscribe(const char* topic, MQTTCallback callback) {
  if (!mqttTopicFits(topic, MQTT_TOPIC_CAP)) {
    Log::kvfw(TAG, "mqtt.sub_rejected reason=topic_length len=%u cap=%u",
              topic ? (unsigned)strlen(topic) : 0u, (unsigned)MQTT_TOPIC_CAP);
    return;
  }
  if (!_subs.add(topic, callback)) {
    Log::kvfe(TAG, "mqtt.sub_rejected reason=max_subs max=%d", MQTT_MAX_SUBS);
    return;
  }

  Log::kvf(TAG, "mqtt.sub_registered topic=%s count=%d max=%d",
           topic, _subs.count(), MQTT_MAX_SUBS);

  // Feed keepalive during bulk Lua subscription (>10 s): without this the
  // broker or HAProxy drops the idle TCP connection.
  if (_client.connected()) {
    _client.subscribe(topic);
    _client.loop();
  }
}

// ---------------------------------------------------------------------------

// Match and fire subscriptions: exact, /#, /+ wildcard.
// in: topic, payload. out: none.
void MQTTClient::matchAndDispatch(const char* topic, const char* payload) {
  strncpy(_rxRing[_rxRingHead], topic, sizeof(_rxRing[0]) - 1);
  _rxRing[_rxRingHead][sizeof(_rxRing[0]) - 1] = '\0';
  _rxRingTs[_rxRingHead] = millis();
  _rxRingHead = (_rxRingHead + 1) % RX_RING_SIZE;
  if (_rxRingCount < RX_RING_SIZE) _rxRingCount++;

  _subs.dispatch(topic, payload);
}

void MQTTClient::onMessage(char* topic, uint8_t* payload, unsigned int length) {
  // Heap copy (a large payload overflows the stack), wiped on free since cert.set
  // and secret.set carry keys. PubSubClient reads no more of its buffer: wipe it.
  size_t bufSize = length + 1;
  char* payloadBuf = (char*)secretAlloc(bufSize);
  if (!payloadBuf) return;
  memcpy(payloadBuf, payload, length);
  payloadBuf[length] = '\0';
  mbedtls_platform_zeroize(payload, length);

  _inboundAuth = cliAuthModeFor(_mtlsActive);
#ifdef MQTT_TLS
  _dispatchTlsVerified = _tlsVerified && s_tls.verified();
#else
  _dispatchTlsVerified = false;
#endif
  matchAndDispatch(topic, payloadBuf);
  secretFree(payloadBuf);
}

// ---------------------------------------------------------------------------

// Publish one CLI response. The topic sits outside the command wildcard, so
// unlike the old one it does not echo back as an inbound URC.
// The platform reads both topics, so it must be deployed before firmware
// carrying this change or those devices go mute on CLI.
// A reply over cap is refused and logged: clipped JSON reaches nobody intact.
// in: prefix, resp, payload byte cap (cliReplyCap). out: none.
static void publishCliResponse(const char* prefix, JsonDocument& resp,
                               size_t cap) {
  char topic[CLI_TOPIC_CAP];
  if (!cliResponseTopic(topic, sizeof(topic), prefix)) {
    Log::kvf("MQTT", "mqtt.cli_topic_truncated prefix=%s", prefix);
    return;
  }
  size_t need = measureJson(resp);
  if (need >= cap) {
    Log::kvfw("MQTT", "mqtt.cli_response_refused reason=oversize bytes=%u cap=%u cmd=%s",
              (unsigned)need, (unsigned)cap, resp["cmd"] | "?");
    return;
  }
  char* rp = (char*)malloc(need + 1);
  if (!rp) {
    Log::kvfw("MQTT", "mqtt.cli_response_refused reason=malloc bytes=%u", (unsigned)need);
    return;
  }
  serializeJson(resp, rp, need + 1);
  MQTTClient::publishNow(topic, rp);
  free(rp);
}

// Dispatch a CLI command from the Shell deferred ring. cmd was extracted
// from the topic; payload is heap-owned by the std::function capture.
// Binary-protocol commands (fs.write, fs.cat chunked, cert.set) are
// handled as special cases before the Shell::execute fallthrough.
// in: cmd, payload (may be null), plen. out: none.
void MQTTClient::runCli(const char* cmd, const char* payload, size_t plen,
                        CliAuthMode auth) {
  if (!cmd || strlen(cmd) == 0) return;

  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  const size_t replyCap = cliReplyCap(_bufferOut, _bufferIn, CLI_TOPIC_CAP - 1);
  const size_t pageLimit = cliPageLimit(replyCap);

  // Optional envelope: {"req_id":<id>, "args":<string>}. req_id is echoed
  // on every cli/response for request/response matching on the shared topic.
  // args is unwrapped and replaces payload for all downstream handlers so
  // binary-protocol special cases (fs.write, cert.set) and Shell::execute
  // all see the inner string. Non-envelope payloads fall through untouched.
  // reqDoc stays in scope so reqId and the args view stay valid at every
  // publish site. Its blocks are wiped on free: args may carry a key.
  JsonDocument reqDoc(secretJsonAllocator());
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

  // Authorization gate - see docs/invariants.md. Runs before every handler.
  {
    // req_id-only envelope carries no args - must not fall back to raw JSON.
    const char* authPayload = reqIdOnlyPayload ? nullptr : payload;
    if (!cliAuthzAllowed(auth, cmd, authPayload, _storedCertBroken)) {
      Log::kvfw(TAG, "mqtt.cli_denied cmd=%s auth=%s", cmd,
                auth == CLI_AUTH_MTLS ? "mtls" : "password");
      JsonDocument resp;
      resp["cmd"] = cmd;
      if (hasReqId) resp["req_id"] = reqId;
      resp["ok"] = false;
      resp["output"][0] = "Denied: not permitted on this connection";
      publishCliResponse(prefix, resp, replyCap);
      return;
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
          publishCliResponse(prefix, resp, replyCap);
          goto cleanup;
        }

        fs::FS* fs = Shell::resolveFS(path);
        const char* fsPath = Shell::stripPrefix(path);
        size_t onDisk = 0;
        const bool append = mode[0] == 'a';
        const FsWrite w = Shell::writeChecked(fs, fsPath, append, (const uint8_t*)content,
                                              contentLen, &onDisk);
        if (w == FS_WRITE_OPEN_FAILED) {
          resp["ok"] = false;
          resp["output"][0] = "Failed to open file";
        } else {
          char msg[160];
          resp["ok"] = w == FS_WRITE_OK;
          if (w == FS_WRITE_OK) {
            snprintf(msg, sizeof(msg), "%s %u bytes to %s",
                     append ? "Appended" : "Wrote", (unsigned)onDisk, path);
          } else {
            snprintf(msg, sizeof(msg), "Write failed: %u of %u bytes landed in %s (filesystem full?)",
                     (unsigned)onDisk, (unsigned)contentLen, path);
          }
          resp["output"][0] = msg;
          resp["bytes"] = onDisk;
          Log::kvf("MQTT", "mqtt.fs_write mode=%s ok=%d bytes=%u want=%u path=%s",
                   append ? "append" : "write", w == FS_WRITE_OK, (unsigned)onDisk,
                   (unsigned)contentLen, path);
        }
      }
      publishCliResponse(prefix, resp, replyCap);
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
        publishCliResponse(prefix, resp, replyCap);
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
          publishCliResponse(prefix, resp, replyCap);
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
              // Size the reply around empty data, then send only the bytes
              // whose escaped form fits. length tells the caller where to resume.
              resp["length"] = (int)got;
              resp["done"] = false;
              resp["data"] = "";
              size_t env = measureJson(resp);
              size_t n = cliJsonFit(buf, got, replyCap > env + 1 ? replyCap - env - 1 : 0);
              if (n == 0) {
                resp.remove("length");
                resp.remove("done");
                resp.remove("data");
                resp["ok"] = false;
                resp["output"][0] = got ? "Chunk does not fit the reply buffer" : "Read failed";
              } else {
                resp["length"] = (int)n;
                resp["done"] = (offset + n >= total);
                resp["data"] = JsonString(buf, n);  // by length: a NUL byte is sent, not a cut
              }
              free(buf);
            } else {
              resp["ok"] = false;
              resp["output"][0] = "malloc failed";
            }
          }
          f.close();
        }

        publishCliResponse(prefix, resp, replyCap);
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

      publishCliResponse(prefix, resp, replyCap);
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

      publishCliResponse(prefix, resp, replyCap);
      goto cleanup;
    }

    // Build "<cmd> <payload>" for Shell::execute. "{}" is a no-arg sentinel
    // (the platform sends it for a command without arguments) and
    // req_id-only envelopes are also no-arg. Commands needing structured
    // JSON cannot use this path - add them as special cases above.
    char line[SHELL_LINE_MAX];
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
      // cmd + payload exceeds the shell line buffer; report and bail out
      // rather than silently truncating the command string.
      JsonDocument resp;
      resp["cmd"] = cmd;
      if (hasReqId) resp["req_id"] = reqId;
      resp["ok"] = false;
      resp["output"][0] = "Command line too long for shell - use chunked variant";
      publishCliResponse(prefix, resp, replyCap);
      goto cleanup;
    }

    // Paginate: when the next output line would overflow the page, ship it
    // with more=true and start a fresh page. Final page is more=false.
    // Single-page (common case) is page 0/more=false, same shape old
    // consumers already handle. A line no page holds goes out in pieces, and
    // a page whose last string continues on the next page carries cont=true.
    JsonDocument resp;
    int pageNum = 0;
    bool lineDropped = false;

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
      publishCliResponse(pfxAfter, resp, replyCap);
    };

    JsonArray output = startPage();

    // A page shipped mid-command says ok:true; the last page carries the result.
    // cliNextPiece sizes each piece, test/test_cli_page runs this loop's model.
    ShellResult result = Shell::execute(line, [&](const char* outLine) {
      size_t len = strlen(outLine);
      for (;;) {
        bool split = false;
        size_t n = cliNextPiece(outLine, len, measureJson(resp), pageLimit, &split);
        if (split && output.size() > 0) {
          publishPage(true);
          pageNum++;
          output = startPage();
          continue;
        }
        if (split && n == 0) {
          Log::kvfw(TAG, "mqtt.cli_line_dropped reason=no_room cmd=%s bytes=%u",
                    cmd, (unsigned)len);
          lineDropped = true;
          return;
        }
        output.add(JsonString(outLine, n));
        if (!split) return;
        resp["cont"] = true;
        publishPage(true);
        pageNum++;
        output = startPage();
        outLine += n;
        len     -= n;
      }
    });

    resp["ok"] = (result == ShellResult::Ok) && !lineDropped;
    publishPage(false);
  }

cleanup:
  return;
}

// ---------------------------------------------------------------------------

// Disconnect and let the loop reconnect from the current broker, port,
// user, and password. The subscription table stays, including Lua callbacks.
// in: none. out: none.
void MQTTClient::reconnectWithCurrentConfig() {
  if (_client.connected()) {
    _client.disconnect();
    _connectedSinceMs = 0;
    Log::kvf(TAG, "mqtt.state_change from=connected to=disconnected reason=cfg_reconnect");
  }
  _reinitPending = true;
}

// Re-register core subscriptions (cli/#, OTA, cmd/config) under the current prefix.
// Called after config.reload when mqtt.topic_prefix changed. Does not
// re-register EventBus handlers or reload TLS certs.
void MQTTClient::reinitSubscriptions() {
  // Drop the session before subscribe(). subscribe() pumps the client
  // while it is still connected. The reconnect resubscribes from the table.
  if (_client.connected()) {
    _client.disconnect();
    _connectedSinceMs = 0;
    Log::kvf(TAG, "mqtt.state_change from=connected to=disconnected reason=sub_reinit");
  }

  _subs.reset();

  JsonObject  cfg    = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  char cliTopic[CLI_TOPIC_CAP];
  if (!cliInputSubscription(cliTopic, sizeof(cliTopic), prefix)) {
    Log::kvf("MQTT", "mqtt.cli_topic_truncated prefix=%s", prefix);
  } else if (shellModeMqttAllowed(Shell::mode())) {
    MQTTClient::subscribe(cliTopic, cliInboundHandler);
  }

  OTAUpdate::begin();
  mqttSubscribeCmdConfig();
  _reinitPending = true;
}

void MQTTClient::resubscribeAll() {
  _subs.forEachActive([](const char* topic) {
    _client.subscribe(topic);
    Log::kvf(TAG, "mqtt.sub_resubscribed topic=%s", topic);
  });
}

// ---------------------------------------------------------------------------

void MQTTClient::publishDiscovery() {
  JsonObject cfg = Config::get();
  bool enabled = cfg["mqtt"]["ha_discovery"] | true;
  if (!enabled) {
    Log::info(TAG, "mqtt.ha_discovery_disabled");
    return;
  }

  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  const char* devName = cfg["device"]["friendly_name"] | cfg["device"]["name"] | "Thesada Node";
  // The persistent id keys HA identity, never nodeName(): device.name is an
  // operator label, and editing it would re-key uniq_id and the retained
  // config topics, leaving the old entities orphaned beside new duplicates.
  // Migration: units that published discovery under a device.name keep those
  // retained topics until they are cleared broker-side.
  const char* devId = Identity::deviceId();
  if (!devId || !*devId) {
    Log::warn(TAG, "mqtt.ha_discovery_skipped reason=identity_unavailable");
    return;
  }

  char availTopic[MQTT_TOPIC_CAP];
  if (!formatTopic(availTopic, sizeof(availTopic), "%s/status", prefix)) return;

  auto makeSlug = [](const char* name, char* slug, size_t sz) {
    strncpy(slug, name, sz - 1);
    slug[sz - 1] = '\0';
    for (char* p = slug; *p; p++) { if (*p == ' ') *p = '_'; *p = tolower(*p); }
  };

  auto disc = [&](const char* component, const char* uid, const char* name,
                  const char* stateTopic,
                  const char* unit, const char* devClass, const char* stateClass,
                  const char* entityCategory = nullptr, uint32_t expireAfterS = 0) {
    char topic[MQTT_TOPIC_CAP];
    if (!formatTopic(topic, sizeof(topic), "homeassistant/%s/%s/%s/config",
                     component, devId, uid)) {
      return;
    }

    JsonDocument doc;
    doc["name"] = name;
    doc["stat_t"] = stateTopic;
    doc["uniq_id"] = uid;
    doc["avty_t"] = availTopic;
    if (unit && strlen(unit) > 0)        doc["unit_of_meas"] = unit;
    if (devClass && strlen(devClass) > 0) doc["dev_cla"] = devClass;
    if (stateClass && strlen(stateClass) > 0) doc["stat_cla"] = stateClass;
    if (entityCategory && strlen(entityCategory) > 0) doc["ent_cat"] = entityCategory;
    if (expireAfterS > 0) doc["exp_aft"] = expireAfterS;

    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"] = devId;
    dev["name"] = devName;
    dev["mf"] = "Thesada";
    dev["mdl"] = "Base Node";
    dev["sw"] = FIRMWARE_VERSION;

    String payload;
    serializeJson(doc, payload);
    publishRetained(topic, payload.c_str());
    // Drain TCP between retained publishes; CYD 1024 B buffers overflow
    // without this and spam EAGAIN. No-op but harmless on cellular path.
    if (_client.connected()) _client.loop();
    yield();
  };

  // uid is a topic segment: sized for the device id, a full slug and the
  // longest suffix (_current), so no two entities clip onto one topic.
  char slug[32], uid[IDENTITY_ID_CAP + sizeof(slug) + sizeof("_current")];
  char stBuf[MQTT_TOPIC_CAP];

  // 26.11.1 clipped a long slug's uid at 48 bytes. That config topic is cleared
  // after this pass, so HA drops the old entity, unless this session publishes it.
  std::vector<String> legacyTopics;
  auto noteLegacy = [&]() {
    char legacy[HA_LEGACY_UID_CAP];
    char t[MQTT_TOPIC_CAP];
    if (haLegacyUid(legacy, sizeof(legacy), uid) &&
        formatTopic(t, sizeof(t), "homeassistant/sensor/%s/%s/config", devId, legacy)) {
      legacyTopics.emplace_back(t);
    }
  };

  JsonArray sensors = cfg["temperature"]["sensors"].as<JsonArray>();
  if (sensors) {
    const char* tunit = cfg["temperature"]["unit"] | "C";
    const char* haUnit = (tunit[0] == 'F' || tunit[0] == 'f') ? "\xC2\xB0""F" : "\xC2\xB0""C";
    uint32_t expS = tempExpireAfterS(tempIntervalS(cfg["temperature"]["interval_s"] | 60));
    for (JsonObject s : sensors) {
      const char* sname = s["name"] | "unknown";
      makeSlug(sname, slug, sizeof(slug));
      snprintf(uid, sizeof(uid), "%s_%s_temp", devId, slug);
      noteLegacy();
      if (formatTopic(stBuf, sizeof(stBuf), "%s/sensor/temperature/%s", prefix, slug))
        disc("sensor", uid, sname, stBuf, haUnit, "temperature", "measurement", nullptr, expS);
    }
  }

  JsonArray channels = cfg["ads1115"]["channels"].as<JsonArray>();
  if (channels) {
    for (JsonObject ch : channels) {
      const char* cname = ch["name"] | "unknown";
      makeSlug(cname, slug, sizeof(slug));

      snprintf(uid, sizeof(uid), "%s_%s_current", devId, slug);
      noteLegacy();
      if (formatTopic(stBuf, sizeof(stBuf), "%s/sensor/current/%s", prefix, slug))
        disc("sensor", uid, cname, stBuf, "A", "current", "measurement");

      char pname[64];
      snprintf(pname, sizeof(pname), "%s Power", cname);
      snprintf(uid, sizeof(uid), "%s_%s_power", devId, slug);
      noteLegacy();
      if (formatTopic(stBuf, sizeof(stBuf), "%s/sensor/power/%s", prefix, slug))
        disc("sensor", uid, pname, stBuf, "W", "power", "measurement");
    }
  }

  bool battEnabled = cfg["battery"]["enabled"] | true;
  if (battEnabled) {
    snprintf(uid, sizeof(uid), "%s_battery_percent", devId);
    if (formatTopic(stBuf, sizeof(stBuf), "%s/sensor/battery/percent", prefix))
      disc("sensor", uid, "Battery", stBuf, "%", "battery", "measurement");

    snprintf(uid, sizeof(uid), "%s_battery_voltage", devId);
    if (formatTopic(stBuf, sizeof(stBuf), "%s/sensor/battery/voltage", prefix))
      disc("sensor", uid, "Battery Voltage", stBuf, "V", "voltage", "measurement");

    snprintf(uid, sizeof(uid), "%s_battery_charging", devId);
    if (formatTopic(stBuf, sizeof(stBuf), "%s/sensor/battery/charging", prefix))
      disc("sensor", uid, "Battery Charge State", stBuf, "", "", "");
  }

  snprintf(uid, sizeof(uid), "%s_wifi_rssi", devId);
  if (formatTopic(stBuf, sizeof(stBuf), "%s/sensor/wifi/rssi", prefix))
    disc("sensor", uid, "WiFi RSSI", stBuf, "dBm", "signal_strength", "measurement", "diagnostic");

  snprintf(uid, sizeof(uid), "%s_wifi_ssid", devId);
  if (formatTopic(stBuf, sizeof(stBuf), "%s/sensor/wifi/ssid", prefix))
    disc("sensor", uid, "WiFi SSID", stBuf, "", "", "", "diagnostic");

  snprintf(uid, sizeof(uid), "%s_wifi_ip", devId);
  if (formatTopic(stBuf, sizeof(stBuf), "%s/sensor/wifi/ip", prefix))
    disc("sensor", uid, "WiFi IP", stBuf, "", "", "", "diagnostic");

  {
    char t[MQTT_TOPIC_CAP], v[32];
    if (formatTopic(t, sizeof(t), "%s/sensor/wifi/rssi", prefix)) {
      snprintf(v, sizeof(v), "%d", (int)WiFi.RSSI());
      _client.publish(t, v);
    }
    if (formatTopic(t, sizeof(t), "%s/sensor/wifi/ssid", prefix))
      _client.publish(t, WiFi.SSID().c_str());
    if (formatTopic(t, sizeof(t), "%s/sensor/wifi/ip", prefix))
      _client.publish(t, WiFi.localIP().toString().c_str());
  }


  snprintf(uid, sizeof(uid), "%s_heap_free", devId);
  if (formatTopic(stBuf, sizeof(stBuf), "%s/sensor/heap/free", prefix))
    disc("sensor", uid, "Free Heap", stBuf, "B", "", "measurement", "diagnostic");

  snprintf(uid, sizeof(uid), "%s_heap_min_free", devId);
  if (formatTopic(stBuf, sizeof(stBuf), "%s/sensor/heap/min_free", prefix))
    disc("sensor", uid, "Min Free Heap", stBuf, "B", "", "measurement", "diagnostic");

  snprintf(uid, sizeof(uid), "%s_heap_max_alloc_block", devId);
  if (formatTopic(stBuf, sizeof(stBuf), "%s/sensor/heap/max_alloc_block", prefix))
    disc("sensor", uid, "Max Alloc Block", stBuf, "B", "", "measurement", "diagnostic");

  snprintf(uid, sizeof(uid), "%s_heap_psram_free", devId);
  if (formatTopic(stBuf, sizeof(stBuf), "%s/sensor/heap/psram_free", prefix))
    disc("sensor", uid, "Free PSRAM", stBuf, "B", "", "measurement", "diagnostic");

  snprintf(uid, sizeof(uid), "%s_uptime", devId);
  if (formatTopic(stBuf, sizeof(stBuf), "%s/sensor/uptime", prefix))
    disc("sensor", uid, "Uptime", stBuf, "s", "duration", "total_increasing", "diagnostic");

  // Zero-length retained clears it at the broker and in HA. Not in the manifest.
  unsigned legacyCleared = 0;
  for (const auto& t : legacyTopics) {
    bool live = false;
    for (const auto& r : _retainedTopics) {
      if (r == t) { live = true; break; }
    }
    if (live || !_client.connected()) continue;
    if (_client.publish(t.c_str(), (const uint8_t*)"", 0, true)) legacyCleared++;
    _client.loop();
    yield();
  }
  if (legacyCleared > 0) Log::kvf(TAG, "mqtt.ha_legacy_cleared count=%u", legacyCleared);

  publishHeapStats();

  Log::info(TAG, "mqtt.ha_discovery_published");
}

// ---------------------------------------------------------------------------

// Publish heap/PSRAM/uptime telemetry. Called on a 5 min timer and once
// from publishDiscovery(). Caches free heap in _lastHeapFree for alert
// tagging. Routes through publish() so cellular deployments get the same
// telemetry.
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

  char topic[MQTT_TOPIC_CAP], value[16];

  if (formatTopic(topic, sizeof(topic), "%s/sensor/heap/free", prefix)) {
    snprintf(value, sizeof(value), "%lu", (unsigned long)freeHeap);
    publish(topic, value);
  }

  if (formatTopic(topic, sizeof(topic), "%s/sensor/heap/min_free", prefix)) {
    snprintf(value, sizeof(value), "%lu", (unsigned long)minFree);
    publish(topic, value);
  }

  if (formatTopic(topic, sizeof(topic), "%s/sensor/heap/max_alloc_block", prefix)) {
    snprintf(value, sizeof(value), "%lu", (unsigned long)maxAlloc);
    publish(topic, value);
  }

  if (formatTopic(topic, sizeof(topic), "%s/sensor/heap/psram_free", prefix)) {
    snprintf(value, sizeof(value), "%lu", (unsigned long)freePsram);
    publish(topic, value);
  }

  if (formatTopic(topic, sizeof(topic), "%s/sensor/uptime", prefix)) {
    snprintf(value, sizeof(value), "%lu", (unsigned long)(millis() / 1000));
    publish(topic, value);
  }

  // WiFi diagnostics only. publishDiscovery seeds them on connect; without
  // this refresh they drift stale during long stable sessions.
  if (WiFi.status() == WL_CONNECTED) {
    if (formatTopic(topic, sizeof(topic), "%s/sensor/wifi/rssi", prefix)) {
      snprintf(value, sizeof(value), "%d", (int)WiFi.RSSI());
      publish(topic, value);
    }
    if (formatTopic(topic, sizeof(topic), "%s/sensor/wifi/ssid", prefix))
      publish(topic, WiFi.SSID().c_str());
    if (formatTopic(topic, sizeof(topic), "%s/sensor/wifi/ip", prefix))
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
  for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", hash[i]);
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
  for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", hash[i]);
  out[64] = '\0';
}

void MQTTClient::publishDeviceInfo() {
  if (!_client.connected()) return;

  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";

  const char* board =
#if defined(BOARD_OWB_RESCUE)
    "owb-rescue";
#elif defined(BOARD_S3_CARRIER)
    "s3-carrier";
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

  char topic[MQTT_TOPIC_CAP];
  if (!formatTopic(topic, sizeof(topic), "%s/info", prefix)) return;
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
  // Reject rather than truncate (same policy as fs.write): a clipped
  // payload reaches the platform as malformed JSON after reconnect,
  // which is worse than a logged drop.
  if (strlen(topic)   >= sizeof(_queue[0].topic) ||
      strlen(payload) >= sizeof(_queue[0].payload)) {
    Log::kvfw(TAG, "mqtt.queue_rejected reason=oversize payload_b=%u topic=%s",
              (unsigned)strlen(payload), topic);
    return;
  }
  if (_queueCount == MQTT_QUEUE_SIZE) {
    Log::warn(TAG, "mqtt.queue_full action=drop_oldest");
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

  Log::kvf(TAG, "mqtt.queued count=%d max=%d topic=%s",
           _queueCount, MQTT_QUEUE_SIZE, topic);
}

// ---------------------------------------------------------------------------

void MQTTClient::flushQueue() {
  while (_queueCount > 0) {
    MQTTMessage& msg = _queue[_queueHead];
    if (msg.valid) {
      _client.publish(msg.topic, msg.payload);
      msg.valid = false;
      Log::kvf(TAG, "mqtt.queue_flushed topic=%s", msg.topic);
    }
    _queueHead = (_queueHead + 1) % MQTT_QUEUE_SIZE;
    _queueCount--;
  }
}

// ---------------------------------------------------------------------------

// Last connected() answer from the main loop, for readers on other tasks.
static volatile bool _connCache = false;

bool MQTTClient::connected() {
  bool up = _client.connected();
  _connCache = up;
  return up;
}

bool MQTTClient::connectedCached() { return _connCache; }

bool MQTTClient::mtlsSessionUp() {
  return _client.connected() && _mtlsActive;
}

bool MQTTClient::clientCertPairValid(const char* certPEM, const char* keyPEM) {
#ifdef MQTT_TLS
  return validateClientCertKey(certPEM, keyPEM);
#else
  (void)certPEM; (void)keyPEM;
  return false;
#endif
}

PairCheck MQTTClient::checkStoredClientCert() {
  char* cert = (char*)secretAlloc(CERT_MAX_LEN);
  char* key  = (char*)secretAlloc(CERT_MAX_LEN);
  PairCheck res = PairCheck::NoHeap;
  if (cert && key) {
    res = loadClientCert(cert, key, CERT_MAX_LEN) && clientCertPairValid(cert, key)
              ? PairCheck::Valid : PairCheck::Invalid;
  }
  secretFree(key);
  secretFree(cert);
  // A refused pair opens cert.clear on this session, not only after the next connect().
  _storedCertBroken = storedCertBrokenAfter(res, _storedCertBroken);
  return res;
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
    Log::error(TAG, "mqtt.cert_store_failed reason=nvs_open");
    return false;
  }
  bool ok = true;
  if (certPEM && *certPEM) {
    size_t len = strlen(certPEM);
    if (len >= CERT_MAX_LEN) {
      Log::error(TAG, "mqtt.cert_store_failed reason=cert_too_large");
      ok = false;
    } else if (!prefs.putString(CERT_KEY_CERT, certPEM)) {
      Log::error(TAG, "mqtt.cert_store_failed reason=nvs_put_cert");
      ok = false;
    }
  }
  if (ok && keyPEM && *keyPEM) {
    size_t len = strlen(keyPEM);
    if (len >= CERT_MAX_LEN) {
      Log::error(TAG, "mqtt.cert_store_failed reason=key_too_large");
      ok = false;
    } else if (!prefs.putString(CERT_KEY_KEY, keyPEM)) {
      Log::error(TAG, "mqtt.cert_store_failed reason=nvs_put_key");
      ok = false;
    }
  }
  prefs.end();
  // A fresh pair supersedes whatever verdict the last load reached. Leaving a
  // stale "broken" here keeps cert.clear open to a password session against a
  // cert that is now fine; the next connect recomputes it either way.
  if (ok) _storedCertBroken = false;
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
// out: true when neither half is left, including when both were absent.
bool MQTTClient::clearClientCert() {
  Preferences prefs;
  if (!prefs.begin(CERT_NS, false)) return false;
  bool certGone = !prefs.isKey(CERT_KEY_CERT) || prefs.remove(CERT_KEY_CERT);
  bool keyGone  = !prefs.isKey(CERT_KEY_KEY)  || prefs.remove(CERT_KEY_KEY);
  prefs.end();
  if (!certGone || !keyGone) {
    Log::kvfe(TAG, "mqtt.cert_clear_failed cert_left=%d key_left=%d", !certGone, !keyGone);
    return false;
  }
  // The broken cert is gone, so the recovery permission it granted goes too.
  _storedCertBroken = false;
  return true;
}

void MQTTClient::scheduleCertReboot(const char* reason) {
  _certRebootReason  = reason;
  _certRebootAtMs    = millis() + CERT_REBOOT_DELAY_MS;
  _certRebootPending = true;
}

bool MQTTClient::certRebootPending() { return _certRebootPending; }

void MQTTClient::serviceCertReboot() {
  if (!_certRebootPending || (int32_t)(millis() - _certRebootAtMs) < 0) return;
  Log::kvfw(TAG, "mqtt.cert_reboot reason=%s", _certRebootReason);
  delay(100);
  OTAUpdate::plannedRestart();
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
  if (!certExtractCn(subj, cn, maxLen)) {
    snprintf(cn, maxLen, "(no CN)");
  }

  char* sp = serial;
  char* se = serial + maxLen - 1;
  for (size_t i = 0; i < crt.serial.len && sp + 2 < se; i++) {
    sp += snprintf(sp, 3, "%02x", crt.serial.p[i]);
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
