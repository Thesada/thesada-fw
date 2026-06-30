// thesada-fw - MQTTClient.h
// MQTT with exponential backoff reconnect, ring buffer publish queue,
// and topic-based subscription dispatch.
// TLS-ready: set MQTT_TLS true in config.h to enable
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <functional>
#include <time.h>
#include <vector>
#include <thesada_config.h>

#ifndef MQTT_QUEUE_SIZE
  #define MQTT_QUEUE_SIZE 8
#endif

static constexpr uint8_t MQTT_MAX_SUBS  = 16;  // max MQTT subscriptions (CLI + Lua + modules)

struct MQTTMessage {
  char topic[64];
  char payload[128];
  bool valid;
};

using MQTTCallback = std::function<void(const char* topic, const char* payload)>;

struct MQTTSubscription {
  char topic[96];
  MQTTCallback callback;
  bool active;
};

class MQTTClient {
public:
  static void begin();
  // Restore mqtt.* from the last-good snapshot if a bad config rebooted the
  // device without connecting. Call at boot before begin().
  static void rollbackIfUncommitted();
  // Pure rollback predicate (no NVS/Config), exposed for unit testing.
  // in: last-good JSON + haveLg, the failing-candidate JSON, current JSON.
  // out: true iff cur is the recorded failing config and differs from last-good.
  static bool rollbackDecision(const char* lg, bool haveLg,
                               const char* rbCfg, const char* cur);
  static void reinitSubscriptions();
  static void loop();
  static void tick();  // lightweight keepalive - call during long init phases
  static void publish(const char* topic, const char* payload);
  static void publishRetained(const char* topic, const char* payload);
  static bool   connected();
  static time_t lastPublishTime();  // UTC epoch of last successful publish; 0 if never

  // Subscribe to a topic with a callback. Re-applied on every reconnect.
  // in:  topic    MQTT topic string (wildcards supported)
  // in:  callback fired on match
  // out: none; can be called before connect()
  static void subscribe(const char* topic, MQTTCallback callback);

  // Per-device mTLS client certificate stored in NVS. If both cert and key
  // are present, MQTT connects with mTLS auth (broker extracts CN as
  // username via use_identity_as_username). If absent, falls back to
  // username/password from config.json.
  //
  // Cert/key are PEM-encoded. Max size per NVS blob: ~4000 B.
  // ECDSA P-256 certs ~800 B, keys ~250 B - well within limit.
  //
  // Public so cellular and any other transport can size buffers passed
  // into loadClientCert.
  static constexpr size_t CERT_MAX_LEN = 4000;

  // in:  certPEM / keyPEM  PEM strings; pass nullptr or empty to store only one half
  // out: true on success
  static bool storeClientCert(const char* certPEM, const char* keyPEM);

  // in:  cert / key  caller-allocated buffers, each >= CERT_MAX_LEN bytes
  // out: true if both are present in NVS
  static bool loadClientCert(char* cert, char* key, size_t maxLen);

  // Clears both cert and key from NVS. Fires setOnClientCertCleared hook
  // after a successful clear so transports holding a cached upload can drop it.
  // out: true on success
  static bool clearClientCert();

  // out: true if both cert and key are present in NVS
  static bool hasClientCert();

  // Hook fired from clearClientCert after NVS rows are gone.
  // Cellular installs one so its modem-side cert cache invalidates and
  // the active modem-MQTT session drops, forcing a reconnect under
  // user/pass auth after the clear.
  // in:  fn  closure, or nullptr to clear
  static void setOnClientCertCleared(std::function<void()> fn);

  // Dispatch an inbound MQTT message into the registered subscription callbacks.
  // Wildcard / exact match logic identical to the WiFi PubSubClient onMessage path.
  // in:  topic / payload / length
  // out: none
  static void dispatchInbound(const char* topic, const char* payload, size_t length);

  // Iterate active subscription topics.
  // in:  fn  called with each topic string
  // out: none
  static void forEachSubscription(std::function<void(const char* topic)> fn);

  // Optional forwarder installed by cellular so any subscribe() called at runtime
  // also lands on the cellular MQTT session.
  // in:  fn  closure, or nullptr to clear
  static void setSubscribeForwarder(std::function<void(const char* topic)> fn);

  // Fallback transport hint. When true, publish() routes to the registered
  // publish-forwarder instead of enqueueing on a WiFi-side disconnect;
  // the stale WiFi-side ring is dropped on handoff (level-triggered
  // sensors refire) rather than replayed through the cellular forwarder.
  // in:  active  true = cellular is publishing on this device's behalf
  static void setFallbackPublishing(bool active);

  // Publish the retained-state set this device owns: availability "online",
  // HA discovery configs, /info, retained-topics manifest.
  // Routes through publishRetained so the cellular forwarder picks it up
  // when WiFi MQTT is down.
  // The session-flag guards against republishing on every cellular
  // STANDBY -> ACTIVE bounce within a single fallback window; it clears
  // on setFallbackPublishing(false) so a new fallback window republishes.
  // in:  force  skip the session-flag check (WiFi reconnect always republishes)
  // out: none
  static void publishRetainedSet(bool force = false);

  // Register a fallback publish forwarder. When WiFi MQTT is down and fallback
  // publishing is active, publish() calls fn so every publish reaches the broker
  // over a single canonical path. fn returns false if the transport is not ready;
  // in that case publish() drops rather than enqueueing on the WiFi-side ring.
  // in:  fn  forwarder closure, or nullptr to clear
  // out: none
  static void setPublishForwarder(std::function<bool(const char* topic, const char* payload, bool retain)> fn);

  // in:  cn / serial / notBefore / notAfter  caller-allocated, each >= maxLen bytes
  // out: true if cert parsed; never touches private key
  static bool getCertInfo(char* cn, char* serial, char* notBefore, char* notAfter, size_t maxLen);

private:
  static void connect();
  static void snapshotGoodConfig();  // commit current critical mqtt cfg as last-good
  static void flushQueue();
  static void enqueue(const char* topic, const char* payload);
  static void onMessage(char* topic, uint8_t* payload, unsigned int length);
  // Capture topic in the rxRing and fire every active subscription whose
  // topic matches by exact string OR trailing /# (multi-level wildcard)
  // OR trailing /+ (single-level wildcard). Shared by onMessage (WiFi)
  // and dispatchInbound (cellular) so wildcard semantics stay in one place.
  // payload must be null-terminated.
  static void matchAndDispatch(const char* topic, const char* payload);
  static void resubscribeAll();
  static void publishDiscovery();
  static void publishHeapStats();
  static void publishDeviceInfo();

  // Tracks every retained topic this device publishes during a connect cycle
  // so the platform can clear them all on device delete.
  // in: topic string. out: appended to _retainedTopics if not already present.
  static std::vector<String> _retainedTopics;
  static bool     _manifestPublished;     // initial manifest emitted at least once
  static bool     _manifestDirty;         // a topic was added after last emit
  static uint32_t _manifestDirtySinceMs;  // first dirty-set timestamp for debounce
  static bool     _retainedPublishedThisSession;  // gate cellular republish bouncing
  static void recordRetainedTopic(const char* topic);
  static void publishRetainedManifest();

public:
  // Sampled at the most recent publishHeapStats() call, or live if never.
  static uint32_t currentFreeHeap();

#ifdef MQTT_TLS
  static WiFiClientSecure _wifiClient;
#else
  static WiFiClient       _wifiClient;
#endif
  static PubSubClient  _client;

  static MQTTMessage   _queue[MQTT_QUEUE_SIZE];
  static uint8_t       _queueHead;
  static uint8_t       _queueTail;
  static uint8_t       _queueCount;

  static MQTTSubscription _subs[MQTT_MAX_SUBS];
  static uint8_t          _subCount;

  static uint32_t      _lastAttempt;
  static uint32_t      _retryInterval;
  static uint8_t       _retryCount;
  // True once the broker-exhaustion reboot loop has been halted this boot:
  // the NVS reboot counter reached mqtt.max_exhaust_reboots, so the device
  // stays alive and keeps retrying without rebooting. Cleared by a successful
  // connect.
  static bool          _rebootHalted;
  static uint32_t      _lastPublishMs;
  static uint32_t      _minIntervalMs;
  static time_t        _lastPublishTime;
  static uint16_t      _bufferIn;
  static uint16_t      _bufferOut;

  // in:  cmd string, payload buffer (may be nullptr), payload length
  // out: publishes to <prefix>/cli/response
  static void          runCli(const char* cmd, const char* payload, size_t plen);

  static uint32_t      _lastSuccessMs;      // millis() of last successful publish or MQTT loop
  static uint32_t      _connectedSinceMs;   // millis() when current connection was established
  static bool          _insecureFallback;   // true if connected without cert validation (NTP not synced)

  static uint32_t      _lastHeapPublishMs;  // millis() of last heap stats publish
  static uint32_t      _lastHeapFree;       // last sampled ESP.getFreeHeap() for alert tagging

  // Preventive reboot watchdog: track when free heap first dropped below
  // HEAP_REBOOT_FLOOR_BYTES. If it stays under for HEAP_REBOOT_HOLD_MS, the
  // device reboots rather than crashing in a malloc inside the TLS stack.
  // 0 means heap is currently above the floor.
  static uint32_t      _lowHeapSinceMs;

  // Debug RX ring - last 8 received topics + timestamps, oldest-overwrite.
  // Helps diagnose broker-side delivery vs client-side dispatch bugs.
#ifndef MQTT_RX_RING_SIZE
  #define MQTT_RX_RING_SIZE 8
#endif
  static constexpr uint8_t RX_RING_SIZE = MQTT_RX_RING_SIZE;
  static char     _rxRing[RX_RING_SIZE][96];
  static uint32_t _rxRingTs[RX_RING_SIZE];
  static uint8_t  _rxRingHead;
  static uint8_t  _rxRingCount;

  static bool          _reinitPending;

public:
  // Deferred reboot latch set by cert.apply. Main loop calls ESP.restart()
  // once the deadline passes so the shell handler can publish its response
  // before the reboot wipes the session. Unconditional reboot is the only
  // reliable recovery when WiFiClientSecure holds sticky mbedtls state
  // across cert swap (classic-platform boards hit this).
  static bool     _certApplyRebootPending;
  static uint32_t _certApplyRebootAtMs;
private:

  static constexpr uint32_t RETRY_MIN_MS   = 2000;
  static constexpr uint32_t RETRY_MAX_MS   = 60000;
  static constexpr uint32_t WATCHDOG_MS    = 600000;
  static constexpr uint32_t HEAP_PUBLISH_MS = 300000;
};
