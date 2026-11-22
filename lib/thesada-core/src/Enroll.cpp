// thesada-fw - Enroll.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "Enroll.h"
#include <thesada_config.h>
#include "Log.h"
#include "Shell.h"

static const char* TAG = "Enroll";

#ifdef ENABLE_IDENTITY

#include "enroll_commit_policy.h"
#include "enroll_policy.h"
#include "mqtt_topic_policy.h"
#include "Config.h"
#include "Identity.h"
#include "MQTTClient.h"
#include "Secret.h"
#include "SecretJson.h"
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

static_assert(ENROLL_TASK_BUDGET_MS + ENROLL_IO_TIMEOUT_MS < ENROLL_JOB_CEILING_MS,
              "the enroll task must end inside its TLS hold");
static_assert(ENROLL_HOST_CAP <= MQTT_BROKER_HOST_CAP,
              "an enrolled broker host must fit MQTTClient's host buffer");

static const char* NS    = "thesada-enroll";
static const char* K_ACK = "ack_pending";
// Staged mqtt fields of a cert store in flight. K_STG is the marker, set last.
static const char* K_STG        = "stg";
static const char* K_STG_HOST   = "stg_host";
static const char* K_STG_PORT   = "stg_port";
static const char* K_STG_PREFIX = "stg_prefix";

static constexpr size_t   BODY_CAP        = 384;
static constexpr size_t   REPLY_CAP       = 6144;
static constexpr uint32_t JOB_CEILING_MS  = ENROLL_JOB_CEILING_MS;
static constexpr uint32_t TASK_STACK      = 12288;

// Local outcomes recorded where an HTTP status would be.
static constexpr int ST_LOCAL    = -1;  // no request went out, or it overran
static constexpr int ST_OVERSIZE = -2;  // reply larger than REPLY_CAP
static constexpr int ST_UNUSABLE = -3;  // 200 whose body could not be applied

// One POST. Allocated by startJob, filled by enrollTask, freed by finishJob.
struct EnrollJob {
  char          url[ENROLL_URL_CAP + 32];
  char          body[BODY_CAP];
  String        reply;
  int           status        = ST_LOCAL;
  bool          isStatusCheck = false;
  bool          pinned        = false;  // holds the MQTT reconnect until the task ends
  volatile bool done          = false;
};

// Reads as closed once the task's deadline passed, so every HTTPClient loop
// (headers, body, chunks) ends inside the job's TLS hold.
class DeadlineTls : public WiFiClientSecure {
 public:
  explicit DeadlineTls(uint32_t startMs) : _start(startMs) {}
  int available() override { return spent() ? 0 : WiFiClientSecure::available(); }
  int read() override { return spent() ? -1 : WiFiClientSecure::read(); }
  int read(uint8_t* buf, size_t size) override { return spent() ? -1 : WiFiClientSecure::read(buf, size); }
  uint8_t connected() override { return spent() ? 0 : WiFiClientSecure::connected(); }
  bool spent() const { return enrollTaskBudgetSpent(millis(), _start); }

 private:
  uint32_t _start;
};

// Revocation status check. Shares the job slot with enrollment, which only
// runs while no cert is stored, so the two never contend.
struct StatusCheck {
  bool     due        = false;
  uint32_t at         = 0;
  uint8_t  failures   = 0;
  int      lastStatus = 0;
  char     serial[ENROLL_SERIAL_HEX_CAP] = "";
};

static bool        _active     = false;
static EnrollStep  _step       = EnrollStep::Done;
static uint8_t     _failures   = 0;
static uint32_t    _nextAt     = 0;
static int         _lastStatus = 0;
static EnrollJob*  _job        = nullptr;
static EnrollJob*  _stale      = nullptr;
static uint32_t    _jobAt      = 0;
static char        _url[ENROLL_URL_CAP];
static char        _sigHex[ENROLL_SIG_HEX_LEN + 1];
static StatusCheck _check;

// Queue the next revocation status check. in: delay. out: none.
static void scheduleCheck(uint32_t delayMs) {
  _check.due = true;
  _check.at  = millis() + delayMs;
}

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
  const bool pinned = job->pinned;
  {
    String ca;
    File f = LittleFS.open("/ca.crt", "r");
    if (f) { ca = f.readString(); f.close(); }
    DeadlineTls tls(millis());
    tls.setHandshakeTimeout(ENROLL_IO_TIMEOUT_MS / 1000);
    tls.setCACert(ca.length() ? ca.c_str() : OTA_CA_PROGMEM);
    HTTPClient http;
    if (http.begin(tls, job->url)) {
      http.setConnectTimeout(ENROLL_IO_TIMEOUT_MS);
      http.setTimeout(ENROLL_IO_TIMEOUT_MS);
      http.addHeader("Content-Type", "application/json");
      job->status = http.POST((uint8_t*)job->body, strlen(job->body));
      if (job->status < 0) {
        char eb[64];
        Log::kvfw(TAG, "enroll.post_failed status=%d tls_err=%d", job->status, tls.lastError(eb, sizeof(eb)));
      }
      if (job->status == 200) {
        if (http.getSize() > (int)REPLY_CAP) {
          job->status = ST_OVERSIZE;
        } else {
          job->reply = http.getString();
          if (job->reply.length() > REPLY_CAP) { wipeString(job->reply); job->status = ST_OVERSIZE; }
        }
      }
      if (tls.spent()) {
        wipeString(job->reply);
        job->status = ST_LOCAL;
        Log::kvfw(TAG, "enroll.task_deadline budget_ms=%u", (unsigned)ENROLL_TASK_BUDGET_MS);
      }
      http.end();
      tls.stop();
    }
  }
  if (pinned) MQTTClient::unpinTlsHold();
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
    scheduleCheck(ENROLL_STATUS_EVERY_MS);
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

// Disk mqtt fields equal these. in: host, port, prefix. out: true on a match.
static bool diskMqttMatches(const char* host, long port, const char* prefix) {
  JsonDocument disk;
  if (!Config::copyDiskDoc(disk)) return false;
  JsonObject m = disk["mqtt"];
  return strcmp(m["broker"] | "", host) == 0 && (long)(m["port"] | 0L) == port &&
         strcmp(m["topic_prefix"] | "", prefix) == 0;
}

// NVS and config steps for enroll_commit_policy.h. Main loop only.
struct EnrollStore {
  char        host[ENROLL_HOST_CAP]           = "";
  char        prefix[Config::TOPIC_PREFIX_CAP] = "";
  long        port    = 0;
  const char* cert    = nullptr;
  const char* key     = nullptr;
  bool        loaded  = false;  // fields usable for configApply
  bool        compare = false;  // skip the write when disk already matches

  bool ackSet(bool v) { return ackPendingSet(v); }
  bool ackPending() { return ackPendingGet(); }

  bool stagePut() {
    Preferences p;
    if (!p.begin(NS, false)) return false;
    bool ok = p.putString(K_STG_HOST, host) > 0 && p.putInt(K_STG_PORT, (int32_t)port) > 0 &&
              p.putString(K_STG_PREFIX, prefix) > 0 && p.putUChar(K_STG, 1) > 0;
    p.end();
    return ok;
  }

  bool stagePresent() {
    Preferences p;
    if (!p.begin(NS, true)) return false;
    bool v = p.getUChar(K_STG, 0) != 0;
    p.end();
    return v;
  }

  bool stageClear() {
    Preferences p;
    if (!p.begin(NS, false)) return false;
    bool ok = !p.isKey(K_STG) || p.remove(K_STG);
    if (ok) {
      if (p.isKey(K_STG_HOST))   p.remove(K_STG_HOST);
      if (p.isKey(K_STG_PORT))   p.remove(K_STG_PORT);
      if (p.isKey(K_STG_PREFIX)) p.remove(K_STG_PREFIX);
    }
    p.end();
    return ok;
  }

  // Read the staged fields back. in: none. out: true when they are usable.
  bool stageLoad() {
    Preferences p;
    if (!p.begin(NS, true)) return false;
    bool got = p.getString(K_STG_HOST, host, sizeof(host)) > 0 &&
               p.getString(K_STG_PREFIX, prefix, sizeof(prefix)) > 0;
    port = p.getInt(K_STG_PORT, 0);
    p.end();
    loaded = got && enrollHostUsable(host) && port >= 1 && port <= 65535 &&
             enrollPrefixUsable(prefix, sizeof(prefix));
    return loaded;
  }

  bool certStore() { return MQTTClient::storeClientCert(cert, key); }
  bool certClear() { return MQTTClient::clearClientCert(); }
  bool certUsable() { return MQTTClient::checkStoredClientCert() == PairCheck::Valid; }

  bool configApply(bool& changed) {
    changed = true;
    if (!loaded) return false;
    if (compare && diskMqttMatches(host, port, prefix)) {
      changed = false;
      return true;
    }
    return Config::edit([&](JsonObject cfg) {
      JsonObject m = cfg["mqtt"].is<JsonObject>() ? cfg["mqtt"].as<JsonObject>()
                                                  : cfg["mqtt"].to<JsonObject>();
      m["broker"]       = String(host);
      m["port"]         = port;
      m["topic_prefix"] = String(prefix);
      return true;
    });
  }
};

// Store a cert reply only when every field is usable. Cert first, config
// last, see enroll_commit_policy.h. in: reply body. out: true when stored.
static bool onCert(const String& reply) {
  JsonDocument doc(secretJsonAllocator());  // key_pem: blocks wiped on free
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
  EnrollStore st;
  strncpy(st.host, host, sizeof(st.host) - 1);
  strncpy(st.prefix, prefix, sizeof(st.prefix) - 1);
  st.port   = port;
  st.cert   = cert;
  st.key    = key;
  st.loaded = true;
  EnrollCommit res = enrollCommit(st);
  if (res == EnrollCommit::Left) {
    // A cert that would not clear: the boot path finishes or undoes it.
    Log::error(TAG, "enroll.store_left action=reboot");
    MQTTClient::scheduleCertReboot("enroll_store_left");
    return false;
  }
  if (res != EnrollCommit::Stored) {
    Log::error(TAG, "enroll.store_failed");
    return false;
  }
  MQTTClient::scheduleCertReboot("cert_stored");
  Log::kvf(TAG, "enroll.cert_stored broker=%s port=%ld reboot_in_ms=%lu",
           host, port, (unsigned long)MQTTClient::CERT_REBOOT_DELAY_MS);
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

static void finishCheck(EnrollJob* job);

// Take the finished POST off the task and advance. in: none. out: none.
static void finishJob() {
  EnrollJob* job = _job;
  _job = nullptr;
  if (job->isStatusCheck) { finishCheck(job); return; }
  int status = job->status;
  if (status == 200 && !onOk(_step, job->reply)) status = ST_UNUSABLE;
  mbedtls_platform_zeroize(job->body, sizeof(job->body));
  wipeString(job->reply);
  delete job;
  if (_step == EnrollStep::Verify) mbedtls_platform_zeroize(_sigHex, sizeof(_sigHex));
  advance(status);
}

// Free a timed-out job once its task has finished. in: none. out: none.
static void reapStale() {
  if (!_stale || !_stale->done) return;
  mbedtls_platform_zeroize(_stale->body, sizeof(_stale->body));
  wipeString(_stale->reply);
  delete _stale;
  _stale = nullptr;
}

// Build one POST and run it on the enroll task. in: none. out: none.
static void startJob() {
  if (_stale) return;
  EnrollJob* job = new (std::nothrow) EnrollJob();
  if (!job) { advance(ST_LOCAL); return; }
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
  const bool yielded = MQTTClient::yieldTlsHeap("enroll", JOB_CEILING_MS);
  // A pin, not the shared timer alone: a net.http release clears that timer.
  job->pinned = yielded;
  if (yielded) MQTTClient::pinTlsHold();
  if (xTaskCreate(enrollTask, "enroll", TASK_STACK, job, tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
    if (yielded) MQTTClient::unpinTlsHold();
    if (yielded) MQTTClient::releaseTlsHeap();
    mbedtls_platform_zeroize(job->body, sizeof(job->body));
    delete job;
    advance(ST_LOCAL);
    return;
  }
  _job   = job;
  _jobAt = millis();
}

// Record a check that got no usable answer and back off. in: status. out: none.
static void checkUnanswered(int status) {
  if (_check.failures < 255) _check.failures++;
  _check.lastStatus = status;
  uint32_t d = enrollBackoffMs(_check.failures, esp_random());
  scheduleCheck(d);
  Log::kvfw(TAG, "enroll.status_unanswered status=%d next_s=%lu failures=%u",
            status, (unsigned long)(d / 1000), (unsigned)_check.failures);
}

// Sign and send the revocation status check for the stored cert.
// in: none. out: none.
static void startCheck() {
  _check.due = false;
  CertInfo info;
  if (!MQTTClient::getCertInfo(info) ||
      !enrollSerialCanonical(info.serial, _check.serial, sizeof(_check.serial))) {
    // No readable cert: nothing to check until the next boot decides again.
    Log::kvfw(TAG, "enroll.status_skipped reason=no_cert");
    return;
  }
  long long ts = (long long)time(nullptr);
  char msg[128];
  if (!enrollStatusMessage(msg, sizeof(msg), Identity::deviceId(), _check.serial, ts)) {
    checkUnanswered(ST_LOCAL);
    return;
  }
  uint8_t sig[64];
  char    sigHex[ENROLL_SIG_HEX_LEN + 1];
  bool signedOk = Identity::sign((const uint8_t*)msg, strlen(msg), sig) &&
                  identityHexEncode(sig, sizeof(sig), sigHex, sizeof(sigHex));
  EnrollJob* job = signedOk ? new (std::nothrow) EnrollJob() : nullptr;
  if (job) job->isStatusCheck = true;
  bool built = job &&
               enrollEndpoint(_url, ENROLL_STATUS_SUFFIX, job->url, sizeof(job->url)) &&
               enrollStatusBody(job->body, sizeof(job->body), Identity::deviceId(),
                                Identity::publicKeyHex(), _check.serial, ts, sigHex);
  const bool yielded = built && MQTTClient::yieldTlsHeap("enroll.status", JOB_CEILING_MS);
  if (yielded) {
    job->pinned = true;
    MQTTClient::pinTlsHold();
  }
  if (!built ||
      xTaskCreate(enrollTask, "enroll", TASK_STACK, job, tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
    if (yielded) MQTTClient::unpinTlsHold();
    if (yielded) MQTTClient::releaseTlsHeap();
    delete job;
    checkUnanswered(ST_LOCAL);
    return;
  }
  _job   = job;
  _jobAt = millis();
}

// Apply a status answer: wipe and reboot on revoked, otherwise reschedule.
// in: finished job (owned here). out: none.
static void finishCheck(EnrollJob* job) {
  int  status  = job->status;
  bool revoked = false;
  const char* answer = "other";  // fixed words only: the reply is not log-safe
  if (status == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, job->reply)) {
      status = ST_UNUSABLE;
    } else {
      const char* st = doc["status"] | "";
      if (strcmp(st, "active") == 0) answer = "active";
      else if (strcmp(st, "unknown") == 0) answer = "unknown";
      revoked = enrollStatusRevoked(status, st, doc["serial"] | "", _check.serial);
    }
  }
  wipeString(job->reply);
  delete job;
  if (status != 200) { checkUnanswered(status); return; }
  _check.failures   = 0;
  _check.lastStatus = status;
  if (revoked) {
    Log::kvfw(TAG, "enroll.cert_revoked serial=%s", _check.serial);
    if (MQTTClient::clearClientCert()) {
      ackPendingSet(false);
      MQTTClient::scheduleCertReboot("cert_revoked");
      return;
    }
    Log::error(TAG, "enroll.cert_clear_failed");
    checkUnanswered(ST_UNUSABLE);
    return;
  }
  uint32_t d = enrollStatusCadenceMs(esp_random());
  scheduleCheck(d);
  Log::kvf(TAG, "enroll.status_kept answer=%s next_s=%lu", answer, (unsigned long)(d / 1000));
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
  if (_check.due) {
    long checkWait = (long)(int32_t)(_check.at - millis()) / 1000;
    snprintf(line, sizeof(line), "check_in_s:  %ld", checkWait < 0 ? 0L : checkWait);
  } else {
    snprintf(line, sizeof(line), "check_in_s:  (off)");
  }
  out(line);
  snprintf(line, sizeof(line), "check_last:  %d", _check.lastStatus);       out(line);
}

// Finish or undo a cert store a reset cut short. Before the start step.
// in: none. out: false when it is still half done and enrollment waits a boot.
static bool recoverStore() {
  EnrollStore st;
  st.compare = true;
  if (st.stagePresent()) st.stageLoad();
  switch (enrollRecover(st)) {
    case EnrollRecover::Idle:
      return true;
    case EnrollRecover::Kept:
      Log::kvf(TAG, "enroll.store_recovered action=kept");
      return true;
    case EnrollRecover::RolledForward:
      Log::kvfw(TAG, "enroll.store_recovered action=forward reboot_in_ms=%lu",
                (unsigned long)MQTTClient::CERT_REBOOT_DELAY_MS);
      MQTTClient::scheduleCertReboot("enroll_store_forward");
      return true;
    case EnrollRecover::RolledBack:
      Log::kvfw(TAG, "enroll.store_recovered action=back");
      return true;
    case EnrollRecover::Left:
      break;
  }
  Log::kvfe(TAG, "enroll.store_recover_failed");
  return false;
}

void Enroll::begin() {
  Shell::registerCommand("enroll.status", "First-boot enrollment step and next attempt",
                         cmd_enroll_status);
  if (!recoverStore()) return;
  if (!Identity::canMint() || !Identity::deviceId()[0]) {
    Log::kvfw(TAG, "enroll.disabled reason=no_identity");
    return;
  }
  bool hasCert    = MQTTClient::hasClientCert();
  bool ackPending = ackPendingGet();
  if (!hasCert && ackPending) ackPendingSet(false);
  _step = enrollStartStep(hasCert, ackPending);

  const char* url   = Config::get()["enroll"]["url"] | "";
  bool        urlOk = enrollUrlUsable(url);
  if (urlOk) {
    strncpy(_url, url, sizeof(_url) - 1);
    _url[sizeof(_url) - 1] = '\0';
  }
  char code[Secret::MAX_LEN];
  bool haveCode = urlOk && enrollLoadClaimCode(code, sizeof(code));
  mbedtls_platform_zeroize(code, sizeof(code));

  if (_step == EnrollStep::Done) {
    // Only a unit that can enroll again is checked: a wipe with no way back
    // strands it, which is worse than holding a cert that no longer works.
    if (haveCode) scheduleCheck(ENROLL_STATUS_FIRST_MS);
    return;
  }
  if (!urlOk) {
    Log::kvfw(TAG, "enroll.disabled reason=no_url");
    return;
  }
  if (!haveCode) {
    Log::kvfw(TAG, "enroll.disabled reason=no_claim_code");
    return;
  }
  _active = true;
  _nextAt = millis();
  Log::kvf(TAG, "enroll.start step=%s url=%s", stepName(_step), _url);
}

void Enroll::loop() {
  reapStale();
  if (_stale) return;
  if (_job) {
    if (_job->done) {
      if (_job->pinned) MQTTClient::releaseTlsHeap();
      finishJob();
    } else if (millis() - _jobAt > JOB_CEILING_MS) {
      // The task still owns the job. Reap it once done; do not start another.
      EnrollJob* overran = _job;
      _stale = _job;
      _job   = nullptr;
      if (overran->isStatusCheck) {
        Log::kvfe(TAG, "enroll.status_overran");
        checkUnanswered(ST_LOCAL);
      } else {
        Log::kvfe(TAG, "enroll.job_overran step=%s", stepName(_step));
        advance(ST_LOCAL);
      }
    }
    return;
  }
  bool rebooting = MQTTClient::certRebootPending();
  if (!_active) {
    // The signed timestamp must be current, not just past the boot floor.
    if (_check.due && !rebooting && WiFiManager::connected() && WiFiManager::ntpSynced() &&
        (int32_t)(millis() - _check.at) >= 0) {
      startCheck();
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
  if (rebooting || !WiFiManager::connected()) return;
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
