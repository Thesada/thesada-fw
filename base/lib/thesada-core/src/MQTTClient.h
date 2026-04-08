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
#include <thesada_config.h>

static constexpr uint8_t MQTT_QUEUE_SIZE = 8;
static constexpr uint8_t MQTT_MAX_SUBS  = 16;  // max MQTT subscriptions (CLI + Lua + modules)

struct MQTTMessage {
  char topic[64];
  char payload[256];
  bool valid;
};

using MQTTCallback = std::function<void(const char* topic, const char* payload)>;

struct MQTTSubscription {
  char topic[96];
  MQTTCallback callback;
  bool active;
};

// Deferred CLI command - stored in callback, executed in loop()
struct DeferredCLI {
  char topic[96];
  char* payload;     // heap-allocated, freed after processing
  uint16_t length;
  bool pending;
};

class MQTTClient {
public:
  static void begin();
  static void loop();
  static void publish(const char* topic, const char* payload);
  static void publishRetained(const char* topic, const char* payload);
  static bool   connected();
  static time_t lastPublishTime();  // UTC epoch of last successful publish; 0 if never

  // Subscribe to a topic with a callback. Subscriptions are stored and
  // re-applied on every reconnect. Can be called before connect().
  static void subscribe(const char* topic, MQTTCallback callback);

private:
  static void connect();
  static void flushQueue();
  static void enqueue(const char* topic, const char* payload);
  static void onMessage(char* topic, uint8_t* payload, unsigned int length);
  static void resubscribeAll();
  static void publishDiscovery();

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

  static DeferredCLI   _deferred;
  static void          processDeferredCLI();

  static uint32_t      _lastSuccessMs;      // millis() of last successful publish or MQTT loop
  static uint32_t      _connectedSinceMs;   // millis() when current connection was established
  static bool          _insecureFallback;   // true if connected without cert validation (NTP not synced)

  static constexpr uint32_t RETRY_MIN_MS   = 2000;
  static constexpr uint32_t RETRY_MAX_MS   = 60000;
  static constexpr uint32_t WATCHDOG_MS    = 600000;  // 10 min - force reconnect if no activity
};
