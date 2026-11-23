// thesada-fw - mqtt_rollback_policy.h
// Pure predicate for the mqtt last-good rollback decision. No NVS/Config deps,
// so it is host-unit-testable. See the rollback invariant for the contract.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <ArduinoJson.h>
#include <string.h>
#include "mqtt_topic_policy.h"

// Roll back to last-good iff a snapshot exists (lg / haveLg), the still-current
// config equals the recorded failing candidate (rbCfg), and it differs from
// last-good. A merely-offline broker (cur == lg) and a recovery edit made after
// an unrelated streak (cur != rbCfg) both return false.
inline bool mqttRollbackShould(const char* lg, bool haveLg,
                               const char* rbCfg, const char* cur) {
  if (!haveLg || !lg || !*lg) return false;   // no snapshot to fall back to
  if (!rbCfg || !*rbCfg) return false;        // no failing candidate recorded
  if (!cur) return false;
  if (strcmp(cur, rbCfg) != 0) return false;  // current != the config that failed
  if (strcmp(cur, lg) == 0) return false;     // current == last-good
  return true;
}

// A connect proves only the broker it dialed. A config whose host or port is not
// the live one (a refused host, a port not applied yet) is not snapshot as last-good.
inline bool mqttSnapshotProven(const char* cfgHost, unsigned cfgPort,
                               const char* liveHost, unsigned livePort) {
  if (!cfgHost || !liveHost || !*liveHost) return false;
  return cfgPort == livePort && strcmp(cfgHost, liveHost) == 0;
}

// Dial only a broker that is set. With no host the failures, and the budget
// reboots they spend, buy something only when a last-good snapshot exists
// for the rollback to restore. in: live host, snapshot set. out: true to dial.
inline bool mqttShouldDial(const char* host, bool haveLg) {
  return (host && *host) || haveLg;
}

// The marker and the snapshot are two NVS writes, so the marker alone proves
// nothing. in: snapshot text, marker. out: true when a rollback could restore it.
inline bool mqttLastGoodUsable(const char* lg, bool lgSet) {
  if (!lgSet || !lg || !*lg) return false;
  JsonDocument d;
  if (deserializeJson(d, lg) || !d.is<JsonObject>()) return false;
  const char* broker = d["broker"] | "";
  return *broker != '\0';
}

// Low-heap defrag reboot: three straight failures that resolved the broker
// and died before it answered (rc -2) on a heap under 40 KB, budget not
// halted. A DNS outage never reached TLS, so it never qualifies.
inline bool mqttDefragRebootDue(unsigned retryCount, bool resolved, int rc,
                                unsigned maxAllocHeap, bool halted) {
  return retryCount >= 3 && resolved && rc == -2 && maxAllocHeap < 40000 && !halted;
}

// DIAL: last-good this session. LIVE: the live config, the fallback is over.
// RETRY: the snapshot ran out of heap, keep the fallback and dial nothing now.
enum MqttSessionLg {
  MQTT_LG_LIVE = 0,
  MQTT_LG_DIAL,
  MQTT_LG_RETRY,
};

// A rollback whose persist failed still dials last-good this session, as long
// as the live critical config is the failing one it replaced. An edit since wins.
// in: persist failed, failing candidate, live critical config, snapshot text.
// out: DIAL with lg parsed and carrying a broker, RETRY on a parse out of heap.
inline MqttSessionLg mqttRollbackSessionDecide(bool persistFailed, const char* failing, const char* cur,
                                               const char* lgText, JsonDocument& lg) {
  if (!persistFailed || !failing || !*failing || !cur || strcmp(failing, cur) != 0) return MQTT_LG_LIVE;
  if (!lgText) return MQTT_LG_LIVE;
  const DeserializationError err = deserializeJson(lg, lgText);
  if (err) return err == DeserializationError::NoMemory ? MQTT_LG_RETRY : MQTT_LG_LIVE;
  if (!lg.is<JsonObject>()) return MQTT_LG_LIVE;
  const char* broker = lg["broker"] | "";
  return *broker != '\0' ? MQTT_LG_DIAL : MQTT_LG_LIVE;
}

// in: as mqttRollbackSessionDecide. out: true only for DIAL.
inline bool mqttRollbackSessionLastGood(bool persistFailed, const char* failing, const char* cur,
                                        const char* lgText, JsonDocument& lg) {
  return mqttRollbackSessionDecide(persistFailed, failing, cur, lgText, lg) == MQTT_LG_DIAL;
}

// KEEP: the endpoint begin() set. SET: the host given. CLEAR: no broker, dial nothing.
enum MqttEndpoint {
  MQTT_EP_KEEP = 0,
  MQTT_EP_SET,
  MQTT_EP_CLEAR,
};

// A fallback that ends on a live config without a broker, or with one past the
// host buffer, must not keep the old host, or live credentials dial it. in: DIAL, fallback was on, host.
inline MqttEndpoint mqttSessionEndpoint(bool lastGood, bool wasFallback, const char* host) {
  if (!lastGood && !wasFallback) return MQTT_EP_KEEP;
  if (host && *host) return mqttHostFits(host, MQTT_BROKER_HOST_CAP) ? MQTT_EP_SET : MQTT_EP_CLEAR;
  return lastGood ? MQTT_EP_KEEP : MQTT_EP_CLEAR;
}
