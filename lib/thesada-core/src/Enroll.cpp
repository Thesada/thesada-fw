// thesada-fw - Enroll.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "Enroll.h"
#include <thesada_config.h>
#include "Log.h"
#include "Shell.h"

static const char* TAG = "Enroll";

#ifdef ENABLE_IDENTITY

#include "enroll_policy.h"
#include "Config.h"
#include "Identity.h"
#include "MQTTClient.h"
#include "Secret.h"
#include "WiFiManager.h"
#include "ota_ca_progmem.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <mbedtls/platform_util.h>
#include <new>

static const char* NS    = "thesada-enroll";
static const char* K_ACK = "ack_pending";

static constexpr size_t   BODY_CAP        = 320;
static constexpr size_t   REPLY_CAP       = 6144;
static constexpr uint32_t JOB_CEILING_MS  = 60000;
static constexpr uint32_t REBOOT_DELAY_MS = 3000;
static constexpr uint32_t TASK_STACK      = 12288;

// One POST. Allocated by startJob, filled by enrollTask, freed by finishJob.
struct EnrollJob {
  char          url[ENROLL_URL_CAP + 32];
  char          body[BODY_CAP];
  String        reply;
  int           status = -1;
  volatile bool done   = false;
};

static bool       _active     = false;
static EnrollStep _step       = EnrollStep::Done;
static uint8_t    _failures   = 0;
static uint32_t   _nextAt     = 0;
static int        _lastStatus = 0;
static EnrollJob* _job        = nullptr;
static uint32_t   _jobAt      = 0;
static bool       _rebootDue  = false;
static uint32_t   _rebootAt   = 0;
static char       _url[ENROLL_URL_CAP];
static char       _sigHex[ENROLL_SIG_HEX_LEN + 1];

// Log name for one step. in: step. out: stable word, or "?" when unknown.
static const char* stepName(EnrollStep s) {
  switch (s) {
    case EnrollStep::Announce:  return "announce";
    case EnrollStep::Verify:    return "verify";
    case EnrollStep::Poll:      return "poll";
    case EnrollStep::AwaitMtls: return "await_mtls";
    case EnrollStep::Ack:       return "ack";
    case EnrollStep::Done:      return "done";
  }
  return "?";
}

// URL suffix for this step's POST. in: step. out: "/verify", "/cert", "/ack", or "".
static const char* stepSuffix(EnrollStep s) {
  switch (s) {
    case EnrollStep::Verify: return "/verify";
    case EnrollStep::Poll:   return "/cert";
    case EnrollStep::Ack:    return "/ack";
    default:                 return "";
  }
}

// Whether a stored cert still owes an ack. in: none. out: true when the flag is set.
static bool ackPendingGet() {
  Preferences p;
  if (!p.begin(NS, false)) return false;
  bool v = p.getBool(K_ACK, false);
  p.end();
  return v;
}

// Store whether a delivered cert still owes an ack.
// in: v. out: true when NVS now holds v.
static bool ackPendingSet(bool v) {
  Preferences p;
  if (!p.begin(NS, false)) return false;
  bool ok = v ? p.putBool(K_ACK, true) > 0 : (!p.isKey(K_ACK) || p.remove(K_ACK));
  p.end();
  return ok;
}

// Copy the seeded claim code. The caller zeroizes out.
// in: out, cap. out: true when it is 8 digits.
bool enrollLoadClaimCode(char* out, size_t cap) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  char nvsKey[16];
  if (!Secret::nvsKeyFor("enroll.claim_code", nvsKey, sizeof(nvsKey))) return false;
  Secret::resolve(nvsKey, "", out, cap);
  return enrollClaimCodeValid(out);
}

// Zero a String's buffer before dropping it. in: s. out: none.
static void wipeString(String& s) {
  if (s.length()) mbedtls_platform_zeroize(&s[0], s.length());
  s = String();
}

// Runs one POST on its own stack: the TLS handshake overflows the 8 KB loop
// task. in: EnrollJob*. out: status and reply filled, done set last.
static void enrollTask(void* arg) {
  EnrollJob* job = static_cast<EnrollJob*>(arg);
  {
    String ca;
    File f = LittleFS.open("/ca.crt", "r");
    if (f) { ca = f.readString(); f.close(); }
    WiFiClientSecure tls;
    tls.setHandshakeTimeout(10);
    tls.setCACert(ca.length() ? ca.c_str() : OTA_CA_PROGMEM);
    HTTPClient http;
    if (http.begin(tls, job->url)) {
      http.setConnectTimeout(10000);
      http.setTimeout(10000);
      http.addHeader("Content-Type", "application/json");
      job->status = http.POST((uint8_t*)job->body, strlen(job->body));
      if (job->status == 200) {
        if (http.getSize() > (int)REPLY_CAP) {
          job->status = -2;
        } else {
          job->reply = http.getString();
          if (job->reply.length() > REPLY_CAP) { wipeString(job->reply); job->status = -2; }
        }
      }
      http.end();
    }
  }
  job->done = true;
  vTaskDelete(nullptr);
}

// Apply one response and log the edge. in: HTTP status, or a negative
// local failure. out: none.
static void advance(int status) {
  EnrollStep   from = _step;
  EnrollAction a    = enrollOnResponse(_step, status, _failures, esp_random());
  if (a.progressed)         _failures = 0;
  else if (_failures < 255) _failures++;
  _step       = a.next;
  _nextAt     = millis() + a.delayMs;
  _lastStatus = status;
  if (a.progressed) {
    Log::kvf(TAG, "enroll.state_change from=%s to=%s status=%d next_s=%lu",
             stepName(from), stepName(_step), status, (unsigned long)(a.delayMs / 1000));
  } else {
    Log::kvfw(TAG, "enroll.state_change from=%s to=%s status=%d next_s=%lu failures=%u",
              stepName(from), stepName(_step), status,
              (unsigned long)(a.delayMs / 1000), (unsigned)_failures);
  }
  if (_step == EnrollStep::Done) {
    _active = false;
    Log::info(TAG, "enroll.sealed");
  }
}

// Announce answer: sign the challenge exactly as sent, as ASCII hex.
// in: reply body. out: true when the signature is stored.
static bool onChallenge(const String& reply) {
  JsonDocument doc;
  if (deserializeJson(doc, reply)) return false;
  const char* ch = doc["challenge"] | "";
  if (!enrollHexValid(ch, ENROLL_CHALLENGE_HEX_LEN)) return false;
  uint8_t sig[64];
  if (!Identity::sign((const uint8_t*)ch, ENROLL_CHALLENGE_HEX_LEN, sig)) return false;
  return identityHexEncode(sig, sizeof(sig), _sigHex, sizeof(_sigHex));
}

// Cert answer: store it only when every field is usable. Order matters: a
// power cut before the cert lands restarts enrollment, never strands a half.
static bool onCert(const String& reply) {
  JsonDocument doc;
  if (deserializeJson(doc, reply)) return false;
  const char* cert   = doc["cert_pem"]     | "";
  const char* key    = doc["key_pem"]      | "";
  const char* id     = doc["device_id"]    | "";
  const char* host   = doc["mqtt_host"]    | "";
  const char* prefix = doc["topic_prefix"] | "";
  long        port   = doc["mqtt_port"]    | 0L;

  if (!enrollReplyUsable(Identity::deviceId(), id, host, port, prefix,
                         Config::TOPIC_PREFIX_CAP)) {
    Log::kvfw(TAG, "enroll.reply_refused reason=fields");
    return false;
  }
  if (strlen(cert) >= MQTTClient::CERT_MAX_LEN || strlen(key) >= MQTTClient::CERT_MAX_LEN ||
      !MQTTClient::clientCertPairValid(cert, key)) {
    Log::kvfw(TAG, "enroll.reply_refused reason=cert_pair");
    return false;
  }
  // A held boot prefix makes save() write the old prefix instead of this one.
  if (Config::topicPrefixHeld()) {
    Log::kvfw(TAG, "enroll.reply_refused reason=prefix_held");
    return false;
  }
  if (!ackPendingSet(true)) return false;

  JsonObject cfg = Config::get();
  JsonObject m   = cfg["mqtt"].is<JsonObject>() ? cfg["mqtt"].as<JsonObject>()
                                                : cfg["mqtt"].to<JsonObject>();
  m["broker"]       = String(host);
  m["port"]         = port;
  m["topic_prefix"] = String(prefix);
  if (!Config::save() || !MQTTClient::storeClientCert(cert, key)) {
    ackPendingSet(false);
    Log::error(TAG, "enroll.store_failed");
    return false;
  }
  _rebootDue = true;
  _rebootAt  = millis() + REBOOT_DELAY_MS;
  Log::kvf(TAG, "enroll.cert_stored broker=%s port=%ld reboot_in_ms=%lu",
           host, port, (unsigned long)REBOOT_DELAY_MS);
  return true;
}

// Handle a 200 for this step. in: step, body. out: true when it was applied.
static bool onOk(EnrollStep step, const String& reply) {
  switch (step) {
    case EnrollStep::Announce: return onChallenge(reply);
    case EnrollStep::Poll:     return onCert(reply);
    case EnrollStep::Ack:      return ackPendingSet(false);
    default:                   return true;
  }
}

// Take the finished POST off the task and advance. in: none. out: none.
static void finishJob() {
  EnrollJob* job = _job;
  _job = nullptr;
  int status = job->status;
  if (status == 200 && !onOk(_step, job->reply)) status = -3;
  mbedtls_platform_zeroize(job->body, sizeof(job->body));
  wipeString(job->reply);
  delete job;
  if (_step == EnrollStep::Verify) mbedtls_platform_zeroize(_sigHex, sizeof(_sigHex));
  advance(status);
}

// Build one POST and run it on the enroll task. in: none. out: none.
static void startJob() {
  EnrollJob* job = new (std::nothrow) EnrollJob();
  if (!job) { advance(-1); return; }
  bool ok = enrollEndpoint(_url, stepSuffix(_step), job->url, sizeof(job->url));
  if (ok && _step == EnrollStep::Verify) {
    ok = enrollBody(job->body, sizeof(job->body), Identity::deviceId(),
                    Identity::publicKeyHex(), "signature", _sigHex);
  } else if (ok) {
    char code[Secret::MAX_LEN];
    ok = enrollLoadClaimCode(code, sizeof(code)) &&
         enrollBody(job->body, sizeof(job->body), Identity::deviceId(),
                    Identity::publicKeyHex(), "claim_token", code);
    mbedtls_platform_zeroize(code, sizeof(code));
  }
  if (!ok) {
    delete job;
    _active = false;
    Log::kvfe(TAG, "enroll.disabled reason=request_unbuildable step=%s", stepName(_step));
    return;
  }
  if (xTaskCreate(enrollTask, "enroll", TASK_STACK, job, tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
    mbedtls_platform_zeroize(job->body, sizeof(job->body));
    delete job;
    advance(-1);
    return;
  }
  _job   = job;
  _jobAt = millis();
}

// Print the enrollment step. in: unused argc/argv, shell writer. out: none.
static void cmd_enroll_status(int, char**, ShellOutput out) {
  char line[192];
  snprintf(line, sizeof(line), "step:        %s", stepName(_step));         out(line);
  snprintf(line, sizeof(line), "active:      %s", _active ? "yes" : "no");  out(line);
  snprintf(line, sizeof(line), "url:         %s", _url[0] ? _url : "(none)"); out(line);
  snprintf(line, sizeof(line), "last_status: %d", _lastStatus);             out(line);
  snprintf(line, sizeof(line), "failures:    %u", (unsigned)_failures);     out(line);
  long wait = _active ? (long)(int32_t)(_nextAt - millis()) / 1000 : 0;
  snprintf(line, sizeof(line), "next_in_s:   %ld", wait < 0 ? 0L : wait);   out(line);
  snprintf(line, sizeof(line), "cert:        %s", MQTTClient::hasClientCert() ? "yes" : "no"); out(line);
}

void Enroll::begin() {
  Shell::registerCommand("enroll.status", "First-boot enrollment step and next attempt",
                         cmd_enroll_status);
  if (!Identity::canMint() || !Identity::deviceId()[0]) {
    Log::kvfw(TAG, "enroll.disabled reason=no_identity");
    return;
  }
  bool hasCert    = MQTTClient::hasClientCert();
  bool ackPending = ackPendingGet();
  if (!hasCert && ackPending) ackPendingSet(false);
  _step = enrollStartStep(hasCert, ackPending);
  if (_step == EnrollStep::Done) return;

  const char* url = Config::get()["enroll"]["url"] | "";
  if (!enrollUrlUsable(url)) {
    Log::kvfw(TAG, "enroll.disabled reason=no_url");
    return;
  }
  strncpy(_url, url, sizeof(_url) - 1);
  _url[sizeof(_url) - 1] = '\0';

  char code[Secret::MAX_LEN];
  bool haveCode = enrollLoadClaimCode(code, sizeof(code));
  mbedtls_platform_zeroize(code, sizeof(code));
  if (!haveCode) {
    Log::kvfw(TAG, "enroll.disabled reason=no_claim_code");
    return;
  }
  _active = true;
  _nextAt = millis();
  Log::kvf(TAG, "enroll.start step=%s url=%s", stepName(_step), _url);
}

void Enroll::loop() {
  if (_rebootDue && (int32_t)(millis() - _rebootAt) >= 0) {
    Log::warn(TAG, "enroll.reboot reason=cert_stored");
    delay(100);
    ESP.restart();
  }
  if (!_active) return;
  if (_job) {
    if (_job->done) {
      finishJob();
    } else if (millis() - _jobAt > JOB_CEILING_MS) {
      // The task may still hold the pointer, so the job is leaked, not freed.
      Log::kvfe(TAG, "enroll.job_overran step=%s", stepName(_step));
      _job = nullptr;
      advance(-1);
    }
    return;
  }
  if (_step == EnrollStep::AwaitMtls) {
    if (MQTTClient::mtlsSessionUp()) {
      _step   = EnrollStep::Ack;
      _nextAt = millis();
      Log::kvf(TAG, "enroll.state_change from=await_mtls to=ack");
    }
    return;
  }
  if (_rebootDue || !WiFiManager::connected()) return;
  if ((int32_t)(millis() - _nextAt) < 0) return;
  startJob();
}

bool Enroll::active() { return _active; }

#else  // !ENABLE_IDENTITY - nothing to sign with, so nothing to enroll.

void Enroll::begin() { Log::kvfw(TAG, "enroll.disabled reason=no_identity_build"); }
void Enroll::loop() {}
bool Enroll::active() { return false; }

bool enrollLoadClaimCode(char* out, size_t) {
  if (out) out[0] = '\0';
  return false;
}

#endif
