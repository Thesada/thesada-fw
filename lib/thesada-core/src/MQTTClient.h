// thesada-fw - MQTTClient.h
// MQTT with exponential backoff reconnect, ring buffer publish queue,
// and topic-based subscription dispatch.
// One session over TlsClient on whichever transport is live: the WiFi
// socket, or the modem socket while cellular has taken over.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <functional>
#include <time.h>
#include <vector>
#include <thesada_config.h>
#include "cert_policy.h"
#include "cli_authz_policy.h"
#include "mqtt_sub_table.h"

#ifndef MQTT_QUEUE_SIZE
  #define MQTT_QUEUE_SIZE 8
#endif

static constexpr uint8_t MQTT_MAX_SUBS  = 16;  // max MQTT subscriptions (CLI + Lua + modules)

// A prefix near Config::TOPIC_PREFIX_CAP plus the longest sensor suffix.
// The queue slot is the truncation point, so it uses the same cap.
static constexpr size_t MQTT_TOPIC_CAP = 160;

struct MQTTMessage {
  // payload sized to cover the 256-byte alert serialization buffer in
  // begin() - alerts are exactly the messages queued during outages.
  char topic[MQTT_TOPIC_CAP];
  char payload[256];
  bool valid;
};

using MQTTCallback = std::function<void(const char* topic, const char* payload)>;

using MQTTSubTable = MqttSubTable<MQTTCallback, MQTT_MAX_SUBS, MQTT_TOPIC_CAP>;

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
  // Disconnect and reconnect from the current broker settings. Does not
  // clear the subscription table.
  static void reconnectWithCurrentConfig();
  static void loop();
  static void tick();  // keepalive for long init phases and blocking shell waits
  static void publish(const char* topic, const char* payload);
  static void publishRetained(const char* topic, const char* payload);
  // Publish outside mqtt.send_interval_s: a CLI reply answers a request now.
  // Queued only while no session is up. out: true when the session took it.
  static bool publishNow(const char* topic, const char* payload);
  // Format a topic into out[cap]. A topic that does not fit is logged and left
  // empty, never clipped. out: true when the whole topic fitted.
  static bool formatTopic(char* out, size_t cap, const char* fmt, ...)
      __attribute__((format(printf, 3, 4)));
  static bool   connected();
  // Any task: the main loop's last connected() answer. Never touches the client,
  // whose connected() can stop() the TLS session the main loop is using.
  static bool   connectedCached();
  static time_t lastPublishTime();  // UTC epoch of last successful publish; 0 if never

  // Subscribe to a topic with a callback. Re-applied on every reconnect.
  // in:  topic    MQTT topic string (wildcards supported)
  // in:  callback fired on match
  // out: none; can be called before connect()
  static void subscribe(const char* topic, MQTTCallback callback,
                        uint8_t owner = MQTT_SUB_OWNER_CORE);
  // Free every slot owner holds and unsubscribe the topics no slot still wants.
  static void unsubscribeOwner(uint8_t owner);

  // Per-device mTLS client certificate stored in NVS. If both cert and key
  // are present, MQTT connects with mTLS auth (broker extracts CN as
  // username via use_identity_as_username). If absent, falls back to
  // username/password from config.json.
  //
  // Cert/key are PEM-encoded. Max size per NVS blob: ~4000 B.
  // ECDSA P-256 certs ~800 B, keys ~250 B - well within limit.
  //
  // Public so callers can size buffers passed into loadClientCert.
  static constexpr size_t CERT_MAX_LEN = 4000;

  // in:  certPEM / keyPEM  PEM strings; pass nullptr or empty to store only one half
  // out: true on success
  static bool storeClientCert(const char* certPEM, const char* keyPEM);

  // in:  cert / key  caller-allocated buffers, each >= CERT_MAX_LEN bytes
  // out: true if both are present in NVS
  static bool loadClientCert(char* cert, char* key, size_t maxLen);

  // Clears both cert and key from NVS.
  // out: true only when neither half is left in NVS
  static bool clearClientCert();

  // out: true if both cert and key are present in NVS
  static bool hasClientCert();

  // A cert change applies only after a restart, so no session keeps running
  // on the old identity. The delay lets the shell reply go out first.
  static constexpr uint32_t CERT_REBOOT_DELAY_MS = 3000;

  // in: reason for the log (static string). out: none.
  static void scheduleCertReboot(const char* reason);

  // out: true once a cert reboot is scheduled; nothing new should start.
  static bool certRebootPending();

  // Restarts once a scheduled cert reboot is due. Called every main-loop
  // tick, whether or not MQTT is enabled. in: none. out: none.
  static void serviceCertReboot();

  // in: PEM cert + key. out: true when both parse and the key matches the cert.
  static bool clientCertPairValid(const char* certPEM, const char* keyPEM);

  // Pair check on the stored halves, copies wiped on free. Records the broken
  // verdict as connect() does. out: Valid, Invalid (will not load or pair), NoHeap.
  static PairCheck checkStoredClientCert();

  // out: true while the broker session is up and presented the client cert
  static bool mtlsSessionUp();

  // Offer the cellular link as the MQTT transport (WiFi down, modem
  // ACTIVE) or withdraw it. WiFi always wins when it is up. On handoff the
  // stale queue is dropped and the next loop dials the new transport.
  // in: active. out: none.
  static void setCellularTransport(bool active);

  // out: "wifi", "cellular" or "none" for the live session.
  static const char* transportName();

  // Disconnect and free the session's TLS buffers. loop() reconnects
  // after the normal retry interval. in: none. out: none.
  static void dropSession();

  // Publish the retained-state set this device owns: availability "online",
  // HA discovery configs, /info, retained-topics manifest. connect() runs
  // it with force on every new session; the session flag stops a
  // non-forced caller from repeating it inside one session.
  // in:  force  skip the session-flag check
  // out: none
  static void publishRetainedSet(bool force = false);

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
  // OR trailing /+ (single-level wildcard).
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
  static bool     _retainedPublishedThisSession;  // retained set already sent this session
  static void recordRetainedTopic(const char* topic);
  static void publishRetainedManifest();

public:
  // Sampled at the most recent publishHeapStats() call, or live if never.
  static uint32_t currentFreeHeap();

  static PubSubClient  _client;

  static MQTTMessage   _queue[MQTT_QUEUE_SIZE];
  static uint8_t       _queueHead;
  static uint8_t       _queueTail;
  static uint8_t       _queueCount;

  static MQTTSubTable  _subs;

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
  // in:  auth  how the delivering session authenticated, frozen at dispatch
  // out: publishes to <prefix>/cli_response
  static void          runCli(const char* cmd, const char* payload, size_t plen,
                              CliAuthMode auth);

  static uint32_t      _lastSuccessMs;      // millis() of last successful publish or MQTT loop
  static uint32_t      _connectedSinceMs;   // millis() when current connection was established

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

private:
  // Deferred cert reboot latch; see scheduleCertReboot.
  static bool        _certRebootPending;
  static uint32_t    _certRebootAtMs;
  static const char* _certRebootReason;

  static constexpr uint32_t RETRY_MIN_MS   = 2000;
  static constexpr uint32_t RETRY_MAX_MS   = 60000;
  static constexpr uint32_t WATCHDOG_MS    = 600000;
  static constexpr uint32_t HEAP_PUBLISH_MS = 300000;
};
