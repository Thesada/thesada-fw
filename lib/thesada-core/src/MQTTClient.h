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
  static void reinitSubscriptions();
  static void loop();
  static void tick();  // lightweight keepalive - call during long init phases
  static void publish(const char* topic, const char* payload);
  static void publishRetained(const char* topic, const char* payload);
  static bool   connected();
  static time_t lastPublishTime();  // UTC epoch of last successful publish; 0 if never

  // Subscribe to a topic with a callback. Subscriptions are stored and
  // re-applied on every reconnect. Can be called before connect().
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
  // into loadClientCert. Was a file-static in MQTTClient.cpp, promoted
  // 2026-05-09 for the SIM7080 modem-MQTT mTLS path (Cellular::writeClientCert).
  static constexpr size_t CERT_MAX_LEN = 4000;
  // storeClientCert writes to NVS. Pass nullptr or empty to store only
  // one half. Returns true on success.
  static bool storeClientCert(const char* certPEM, const char* keyPEM);
  // loadClientCert fills cert and key (both must be non-null buffers of
  // size maxLen >= CERT_MAX_LEN). Returns true if both are present in NVS.
  static bool loadClientCert(char* cert, char* key, size_t maxLen);
  // Clears both cert and key from NVS. Returns true on success.
  // Fires the optional cert-cleared hook (setOnClientCertCleared) after
  // a successful clear so transports holding a cached upload can drop it.
  static bool clearClientCert();
  // True if both cert and key are present in NVS.
  static bool hasClientCert();

  // Optional hook fired from clearClientCert after the NVS rows are gone.
  // The cellular module installs one at bring-up so its modem-side cert
  // cache invalidates and any active modem-MQTT session drops, forcing
  // a reconnect under the new auth mode (user/pass after the clear).
  // Pass nullptr to clear the hook.
  static void setOnClientCertCleared(std::function<void()> fn);

  // Dispatch an inbound MQTT message into the registered subscription
  // callbacks the same way the WiFi PubSubClient onMessage callback does.
  // Used by Cellular::pumpInbound to share one subscription registry across
  // both transports. Wildcard / exact match logic identical.
  static void dispatchInbound(const char* topic, const char* payload, size_t length);

  // Iterate active subscription topics. Callback receives each topic
  // string. Used by Cellular::smsubAll to issue AT+SMSUB on cellular MQTT
  // bring-up so the same set works on both transports.
  static void forEachSubscription(std::function<void(const char* topic)> fn);

  // Optional cross-transport forwarder. Cellular installs a hook here on
  // bring-up so any subscribe() called at runtime (e.g. Lua mqtt.subscribe)
  // also lands on the cellular MQTT session. Pass nullptr to clear.
  static void setSubscribeForwarder(std::function<void(const char* topic)> fn);

  // Fallback transport hint. CellularModule sets this true while cellular
  // MQTT is publishing on this device's behalf. When true, MQTTClient::publish
  // routes to the registered publish-forwarder (see setPublishForwarder)
  // instead of enqueueing on a WiFi-side disconnect; the WiFi-side ring is
  // preserved untouched until WiFi MQTT comes back.
  static void setFallbackPublishing(bool active);

  // Register a fallback publish forwarder. Cellular installs one on
  // bring-up that wraps Cellular::publish; when WiFi MQTT is down and
  // fallback publishing is active, MQTTClient::publish calls the
  // forwarder so every publish (sensors, gnss, cli responses, ad-hoc)
  // reaches the broker over cellular through a single canonical path.
  // Forwarder returns true on accepted-and-shipped, false to mean
  // "transport not ready"; in the false case publish() drops instead
  // of enqueueing (the WiFi-side ring is reserved for WiFi).  Pass
  // nullptr to clear.
  //
  // in:  fn   forwarder closure, or nullptr to clear
  // out: none
  static void setPublishForwarder(std::function<bool(const char* topic, const char* payload, bool retain)> fn);
  // Parse stored cert PEM and fill info (caller-allocated, size >= 128).
  // Fields: CN, serial hex, not_before, not_after, issuer CN.
  // Returns true if cert parsed. Never touches private key.
  static bool getCertInfo(char* cn, char* serial, char* notBefore, char* notAfter, size_t maxLen);

private:
  static void connect();
  static void flushQueue();
  static void enqueue(const char* topic, const char* payload);
  static void onMessage(char* topic, uint8_t* payload, unsigned int length);
  // Capture topic in the rxRing and fire every active subscription whose
  // topic matches by exact string OR trailing /# (multi-level wildcard)
  // OR trailing /+ (single-level wildcard). Shared by onMessage (WiFi)
  // and dispatchInbound (cellular) so both transports use the same
  // matching rules - any future change to wildcard semantics lands in
  // one place. payload must be null-terminated.
  static void matchAndDispatch(const char* topic, const char* payload);
  static void resubscribeAll();
  static void publishDiscovery();
  static void publishHeapStats();
  static void publishDeviceInfo();

  // Tracks every retained topic this device publishes during a connect cycle.
  // Cleared at the start of each connect(), filled by recordRetainedTopic()
  // from each retained-publish site (LWT, /info, HA discovery, modules), then
  // serialized to <prefix>/info/retained_topics by publishRetainedManifest()
  // so the platform can clear them all on device delete.
  // in: topic string. out: appended to _retainedTopics if not already present.
  static std::vector<String> _retainedTopics;
  static bool     _manifestPublished;     // initial manifest emitted at least once
  static bool     _manifestDirty;         // a topic was added after last emit
  static uint32_t _manifestDirtySinceMs;  // first dirty-set timestamp for debounce
  static void recordRetainedTopic(const char* topic);
  static void publishRetainedManifest();

public:
  // Sampled at the most recent publishHeapStats() call, or live if never.
  // Used by alert paths (Telegram tagging) so every outbound alert carries
  // an observability breadcrumb.
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
  static uint32_t      _lastPublishMs;
  static uint32_t      _minIntervalMs;
  static time_t        _lastPublishTime;
  static uint16_t      _bufferIn;
  static uint16_t      _bufferOut;

  // CLI handler invoked on the main loop (Shell::enqueueDeferred drain).
  // Receives the command name extracted from the cli/<cmd> topic and a heap
  // copy of the raw payload. Special-cases binary protocols (fs.write,
  // fs.cat chunked, cert.set) before falling through to Shell::execute.
  // The caller (PubSubClient subscribe lambda) builds the std::function
  // capture and enqueues - this is just the body that runs at drain time.
  // in: cmd string, payload buffer (may be nullptr), payload length.
  // out: publishes to <prefix>/cli/response.
  static void          runCli(const char* cmd, const char* payload, size_t plen);

  static uint32_t      _lastSuccessMs;      // millis() of last successful publish or MQTT loop
  static uint32_t      _connectedSinceMs;   // millis() when current connection was established
  static bool          _insecureFallback;   // true if connected without cert validation (NTP not synced)

  static uint32_t      _lastHeapPublishMs;  // millis() of last heap stats publish
  static uint32_t      _lastHeapFree;       // last sampled ESP.getFreeHeap() for alert tagging

  // Preventive reboot watchdog: track when free heap first dropped below
  // HEAP_REBOOT_FLOOR_BYTES. If it stays under for HEAP_REBOOT_HOLD_MS, the
  // device reboots itself rather than crashing in a malloc inside the TLS
  // stack. 0 means heap is currently above the floor.
  static uint32_t      _lowHeapSinceMs;

  // Debug RX ring - last 8 received topics + timestamps, oldest-overwrite.
  // Used by `net.mqtt rx` to see what actually arrives at onMessage - helps
  // diagnose broker-side delivery vs client-side dispatch bugs.
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
  static constexpr uint32_t WATCHDOG_MS    = 600000;  // 10 min - force reconnect if no activity
  static constexpr uint32_t HEAP_PUBLISH_MS = 300000; // 5 min - periodic heap telemetry
};
