// thesada-fw - Cellular.h
// SIM7080G LTE-M/NB-IoT connectivity with modem-native MQTT over TLS.
// PMU (AXP2101), TinyGSM registration, AT+SM* MQTT stack.
// Network-selection policy (when to activate vs yield to WiFi) lives in
// CellularModule. This class owns the modem itself: bring-up, recovery
// from cellular drops, and a publish gate it can be told to open or close.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class Cellular {
public:
  // RAII guard for the SIM7080 AT bus. Every call site that touches
  // Serial1 / TinyGSM must instantiate one of these at function entry.
  // Replaces the older cooperative `s_atBusy` flag: a real
  // FreeRTOS recursive mutex serializes caller-vs-caller (publish on
  // one task vs the loop() health probe on another) and caller-vs-URC
  // drainer (pumpInbound), which the boolean flag could not.
  //
  // Recursive: nested helpers (networkConnect -> writeCACert etc.) can
  // re-acquire safely without self-deadlock if a future refactor moves
  // the guard down a level.
  //
  // Default 30 s acquire window: long enough that a slow SMCONN inside
  // an outer guard does not livelock another caller waiting on publish,
  // short enough that a wedged AT bus surfaces as a returnable failure
  // rather than an infinite block. Pass 0 for non-blocking try-acquire
  // (used by pumpInbound to step aside without ever waiting).
  class ATGuard {
  public:
    explicit ATGuard(uint32_t timeoutMs = 30000);
    ~ATGuard();
    bool ok() const { return _held; }
    // Temporarily release the AT bus mutex for ~pauseMs and re-acquire
    // before returning. The chunked sleep inside also pumps the shell
    // console + task watchdog, so commands typed during the pause
    // (cell.at, etc) can run interleaved instead of starving the main
    // loop. Caller MUST NOT issue any AT commands or touch Serial1
    // while paused. No-op if the guard does not currently hold the
    // mutex (best-effort - caller still gets the requested delay).
    // Re-acquire budget is fixed at 60 s; if another holder won't
    // release in that window, pause() returns with _held=false and
    // ok() will reflect the loss.
    void pause(uint32_t pauseMs);
    ATGuard(const ATGuard&) = delete;
    ATGuard& operator=(const ATGuard&) = delete;
  private:
    bool _held;
  };

  // Power up the modem only - PMU rails on, PWRKEY pulsed, AT verified.
  // Does NOT register on the network or connect MQTT. Lets the GNSS path
  // bring the modem up without taking over network duty (the SIM7080
  // GNSS receiver shares the modem core; reading it requires the modem
  // be powered, but not registered or MQTT-connected).
  // Idempotent: returns immediately if the modem is already AT-responsive.
  // Caller-safe from any task; takes the AT-bus mutex internally.
  // out: true if modem AT-OK after the call.
  static bool powerOn();

  // Result of one Cellular::tickActivation() call.
  enum class ActStatus {
    PENDING,  // bring-up still walking its phases - poll again next tick
    DONE,     // modem registered + MQTT connected; publish gate openable
    FAILED,   // hard failure (modem dead / SIM absent / activation timeout)
  };

  // Incremental cellular bring-up. Replaces the old blocking begin():
  // each call advances at most one phase of the modem activation state
  // machine (power-on -> SIM -> radio config -> registration -> bearer
  // -> MQTT) and returns fast, so every other module loop keeps ticking
  // between phases. CellularModule polls this each loop while ACTIVATING.
  //
  // Registration is polled one status read per call (~1 Hz). A
  // configurable deadline (cellular.activation_timeout_ms, default
  // 30 min) bounds the whole cycle: on expiry the modem is hardware-
  // reset and FAILED returned, so the device drops back to WiFi-watching
  // standby instead of looping a wedged modem or bad broker forever.
  //
  // Idempotent: returns DONE immediately if cellular is already up
  // (re-entry after a yield). Resets the internal phase to the top on
  // DONE or FAILED so the next ACTIVATING cycle walks from scratch.
  static ActStatus tickActivation();

  // Cellular-only recovery loop: re-register on network drop, reconnect MQTT
  // if dropped while network is up. Does NOT touch WiFi; CellularModule owns
  // the WiFi-vs-cellular selection.
  static void loop();

  // True when modem-MQTT is connected AND the publish gate is open.
  // Used by every Cellular::publish() guard and by external subscribers.
  static bool connected();

  // Open or close the publish gate without disconnecting the modem.
  // Closed gate keeps the MQTT session warm so re-takeover after a brief
  // WiFi flap can resume publishing without a fresh registration cycle.
  static void setPublishGate(bool open);

  // Last cached signal quality from AT+CSQ. 0..31 valid, 99 = unknown.
  // Updated by Cellular::loop() when started; returns 99 before first sample.
  static int  getSignalQuality();

  // Publish via modem-native AT+SMPUB. Returns false if not connected.
  // retain=true sets the AT+SMPUB retain flag so HA discovery, retained-
  // topics manifest, and LWT availability survive a cellular-leg publish
  // the same way they do over WiFi.
  static bool publish(const char* topic, const char* payload, bool retain = false);

  // Send a raw AT command to the SIM7080 modem and stream the response
  // back to `emit`. Bypasses TinyGSM so it can be used at any state, even
  // when the network connect path is mid-flight. Pass `cmd` without the
  // "AT" prefix (e.g. "+CSQ", "+COPS=?"). `timeoutMs` should be generous
  // for slow commands like operator scan (>= 120s).
  static void atPassthrough(const char* cmd, uint32_t timeoutMs,
                            std::function<void(const char*)> emit);

  // True once the modem is powered and AT-responsive (after powerOn() or
  // begin()). Required precondition for any GNSS use - the SIM7080 GNSS
  // receiver shares the same modem core. Registration / MQTT do not
  // need to be up for GNSS.
  static bool isModemAlive();

  // Hardware-reset the modem: PowerManager::resetModem() (deeper DC3 +
  // BLDO2 1 s cycle) followed by PWRKEY pulse and AT-probe wait. Last-
  // resort recovery for the SIM7080G "URC routing wedged" state where
  // SMSTATE / CEREG / CSQ all report healthy but `+SMSUB:` URCs stop
  // surfacing. After this returns, _modemAlive / _started / _mqttConnected
  // are all reset; CellularModule::loop() will re-walk activation on the
  // next tick. Returns true if AT comes back within budget.
  static bool hardReset();

  // Subscribe to an MQTT topic on the cellular modem-native MQTT session
  // (AT+SMSUB). Issued automatically for every entry in MQTTClient::_subs
  // when cellular MQTT comes up; also called from MQTTClient::subscribe()
  // when a subscription is added at runtime while cellular is active.
  static bool smsub(const char* topic);
  // Issue AT+SMSUB for every active subscription registered with MQTTClient.
  // Called from mqttConnect() right after CONN succeeds.
  static bool smsubAll();
  // Drain Serial1 once per loop and dispatch any +SMSUB: URCs through
  // MQTTClient::dispatchInbound. Non-blocking. Intended caller: end of
  // Cellular::loop, after any TinyGSM AT traffic for this tick is done.
  static void pumpInbound();

  // Drop the modem-side cached client cert (next mqttConnect re-uploads
  // from current NVS or falls back to user/pass if NVS is now empty)
  // AND tear down any active modem-MQTT session so it reconnects under
  // the new auth mode. Wired to MQTTClient::setOnClientCertCleared in
  // begin(); also safe to call directly. No-op if cellular MQTT was
  // never up.
  static void invalidateClientCert();

  // HTTPS GET via the modem-native SSL socket layer (CAOPEN + CASSL,
  // wrapped by TinyGsmClientSecure). Used by the cellular-fallback OTA
  // path so a device with WiFi down can still pull firmware updates.
  //
  // Why not the SH HTTP service: SIM7080G firmware 1951B17 implements
  // SHSSL + SHCONF as test commands but rejects every executive SH call
  // (SHSSL, SHCONN, SHREQ) with "CME ERROR: operation not allowed". The
  // lilygo SIM7080G HTTP/HTTPS example uses TinyGsmClientSecure for the
  // same reason - the socket layer is the supported HTTPS path on this
  // fw rev. Coexists with the modem-native MQTT (SM) session because
  // they use independent socket slots.
  //
  // Pre-conditions: modem powered, registered, +CNACT slot 0 activated.
  // The CA cert at /ca.crt on LittleFS must already be uploaded to the
  // modem FS as `server-ca.crt` (writeCACert handles this on first
  // begin()). For host validation TinyGsmClientSecure binds the CA via
  // CSSLCFG slot 0 - same slot MQTT uses for its server-cert auth.
  //
  // `writeCallback` is invoked with each chunk of the response BODY
  // (status line + headers are parsed away). Returning false from the
  // callback aborts the transfer; the socket is still cleanly closed.
  //
  // in:  host         hostname without scheme (e.g. "ota.thesada.app")
  //      path         path with leading slash (e.g. "/latest/foo.json")
  //      port         TCP port (443 for HTTPS)
  //      writeCallback per-chunk body delivery
  //      rangeStart, rangeEndInclusive  optional Range request bounds.
  //                   When rangeEndInclusive > 0, the helper adds
  //                   `Range: bytes=<start>-<end>` to the GET and
  //                   expects a 206 response. Used by the cellular OTA
  //                   path to defeat SIM7080G HTTP-session degradation
  //                   past ~500-900 KB observed on fw 1951B17: each
  //                   chunk is its own short-lived TLS socket so the
  //                   modem-internal state resets between requests.
  // out: httpStatus   HTTP response status code on the wire, or 0 on
  //                   transport failure (connect / TLS handshake / EOF
  //                   before status line)
  // returns: true if the request completed and (when a Content-Length
  //          was advertised) at least that many body bytes reached the
  //          callback. false on transport failure, callback abort, or
  //          short body.
  static bool httpsGet(const char* host, const char* path, uint16_t port,
                       std::function<bool(const uint8_t*, size_t)> writeCallback,
                       int* httpStatus,
                       size_t rangeStart = 0,
                       size_t rangeEndInclusive = 0);

  // GNSS fix acquisition. The SIM7080G time-shares its radio
  // between LTE and GNSS - while CGNSPWR=1 the LTE data path is suspended,
  // so any modem-native MQTT publish issued during that window fails. The
  // correct cycle is CGNSPWR=1 -> wait fix -> CGNSPWR=0 -> CFUN=1, all in
  // one atomic call so the LTE data path is restored before control
  // returns to the caller. TCP/TLS sessions survive the window; the broker
  // delivers anything sent during it once CFUN=1 wakes the LTE side.
  //
  // First cold fix: up to ~60 s. Warm fixes thereafter: a few seconds.
  // Caller must verify isModemAlive() first; otherwise no-op false.
  //
  // in:  timeoutMs    upper bound on fix acquisition window
  //      lat/lon/...  output pointers (any may be nullptr)
  // out: bool         true if a 2D/3D fix was acquired within the window
  static bool gpsAcquireFix(uint32_t timeoutMs,
                            float* lat, float* lon,
                            float* alt = nullptr, float* speed = nullptr,
                            int* satsInView = nullptr, int* satsUsed = nullptr);

  // --- net.* transport-abstraction helpers --------------------------------
  // Wired into Net::CellularProvider by CellularModule::begin() so the
  // thesada-core net.* shell commands keep working on the cellular leg
  // when WiFi is down. Each takes the AT-bus mutex internally and is safe
  // to call from the shell task.

  // True when the modem is registered AND the data context (CNACT slot 0)
  // is active - the precondition for DNS / NTP / HTTPS over the modem.
  static bool dataLinkUp();

  // Close the data context (CNACT slot 0) and modem MQTT session while
  // leaving the modem powered and network-registered. Drops _started so
  // a future re-takeover re-walks tickActivation(). Used on WiFi return when
  // link_mode is "fallback" so an idle PDP context does not burn data.
  static void dataLinkDown();

  // Emit operator (AT+COPS?), modem IP (AT+CNACT?), cached signal quality,
  // and IMEI (AT+GSN) as human-readable lines via `emit`. Best-effort.
  static void netInfo(std::function<void(const char*)> emit);

  // Resolve `host` to a dotted-quad string via AT+CDNSGIP. Returns true
  // and writes a NUL-terminated address into `out` (capped to outLen) on
  // success. Requires the data context to be up (see dataLinkUp).
  static bool resolveHost(const char* host, char* out, size_t outLen);

  // Sync the ESP32 system clock from the modem: AT+CNTP against `server`,
  // then read AT+CCLK? and settimeofday() with the resulting UTC epoch.
  // Recovers the clock when WiFi never associated and the WiFiManager
  // SNTP path never ran. Returns true on success.
  static bool ntpSync(const char* server, uint32_t timeoutMs);

private:
  static void initPMU();
  static bool wakeModem();
  static bool writeCACert();
  // Upload the NVS-stored MQTT client cert + key as a single concatenated
  // PEM file (`client.crt` on the modem FS) for SIM7080 mTLS. Mirrors
  // writeCACert's CFSWFILE chunked write. Returns false if NVS is empty
  // or any AT step fails - caller falls back to non-mTLS path. Safe to
  // call only when an ATGuard is held by an outer scope.
  static bool writeClientCert();
  // Synchronous network bring-up (radio config + blocking registration
  // poll + bearer). Retained for the steady-state recovery path in
  // Cellular::loop() and the modem-wedge path in mqttConnect(). The
  // incremental tickActivation() uses the three helpers below directly
  // so it can yield between phases instead of blocking.
  static bool networkConnect();
  // Radio configuration block: CFUN power-cycle, network/preferred mode,
  // APN (CGDCONT/CNCFG), CEREG/CGREG URC enable, RF-settle delay. Caller
  // owns the AT bus.
  static void radioConfigure();
  // One network-registration status read. Returns 1 on HOME/ROAMING,
  // 0 while still searching, -1 on REG_DENIED. Caller owns the overall
  // registration timeout. Caller owns the AT bus.
  static int  pollRegistration();
  // Activate the PDP data context (CNACT slot 0), falling back to
  // TinyGSM gprsConnect(). Caller owns the AT bus. Returns true once
  // the bearer reports connected.
  static bool activateBearer();
  static bool mqttConnect();
  static bool isRegistered();
  static bool mqttIsConnected();
  static void sampleSignalQuality();

  // URC-safe AT helper. Sends "AT<cmd>\r\n" via Serial1, reads the
  // response line by line. Any +SMSUB: line is routed to dispatchInbound
  // (same path pumpInbound uses) so a URC arriving inside the call's
  // response window is delivered, not eaten by the parser.
  // Works around TinyGSM SIM7080 handleURCs missing +SMSUB: support;
  // lets us drop steady-state TinyGSM AT calls without forking the
  // upstream library.
  //
  // Caller must hold the AT-bus mutex (ATGuard).
  //
  // in:  cmd          AT command without leading "AT" or trailing CRLF
  //                   (e.g. "+CSQ", "+SMSTATE?")
  //      expect       success terminator the caller is waiting for
  //                   (typically "OK")
  //      timeoutMs    upper bound on response window
  //      lineCallback per non-URC, non-terminator line; nullptr ok
  // out: int           1 on expect match, 0 on timeout, -1 on ERROR
  static int  cellAT(const char* cmd, const char* expect, uint32_t timeoutMs,
                     std::function<void(const char*)> lineCallback);

  // URC-safe variant for AT cmds with a "> prompt -> payload -> OK" cycle
  // (AT+SMPUB, AT+CFSWFILE, etc). Sends "AT<cmd>\r\n", waits for the '>'
  // prompt char, writes payload bytes, then waits for the success
  // terminator. Routes any +SMSUB: line found in either wait phase via
  // routeSmsubLine, so URCs arriving during SMPUB are not eaten by
  // TinyGSM's waitResponse (which is the bug that drops 3/5 URCs in a
  // 1 s burst test).
  //
  // Caller must hold the AT-bus mutex (ATGuard).
  //
  // in:  cmd               AT command without leading "AT" or trailing CRLF
  //      payload           bytes to write after the '>' prompt
  //      payloadLen        byte count
  //      promptTimeoutMs   upper bound on '>' prompt wait
  //      okTimeoutMs       upper bound on success terminator wait
  // out: int                1 on OK terminator, 0 on timeout, -1 on ERROR
  static int  cellATWrite(const char* cmd, const uint8_t* payload,
                          size_t payloadLen, uint32_t promptTimeoutMs,
                          uint32_t okTimeoutMs);

  static void routeSmsubLine(char* line);
  static bool isGprsConnectedRaw();

  static bool     _modemAlive;     // PMU + wakeModem ok (powerOn complete)
  static bool     _started;        // tickActivation() reached DONE (MQTT up)
  static bool     _mqttConnected;
  static bool     _publishGate;
  static bool     _hasCACert;
  // True once the NVS client cert + key have been uploaded to the modem
  // FS (`client.crt`) and CSSLCFG-converted. Reset to false on
  // hardReset / modemSoftReset (modem FS clears across power cycle) and
  // on cert.clear via the MQTTClient cert-cleared hook. mqttConnect
  // re-uploads on the next cycle when the flag is false and NVS still
  // has a cert - cheap one-shot cache, avoids the 4-8 KB AT FS write
  // on every reconnect.
  static bool     _hasClientCertOnModem;
  static int      _signalQuality;
  static uint32_t _lastSignalSample;
  static uint32_t _lastSmpubMs;     // 1 Hz rate-limit on AT+SMPUB

  // AT-bus mutex. Lazy-initialised (xSemaphoreCreateRecursiveMutex)
  // on first ATGuard construction so static init order is irrelevant.
  static SemaphoreHandle_t _atMutex;
  static void atMutexInit();
};
