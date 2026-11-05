// thesada-fw - Cellular.h
// SIM7080G LTE-M/NB-IoT connectivity: PMU (AXP2101), TinyGSM registration,
// data bearer and plain TCP sockets. TLS and MQTT run on the ESP32 over
// those sockets (TlsClient, MQTTClient), the same stack WiFi uses.
// Network-selection policy (when to activate vs yield to WiFi) lives in
// CellularModule. This class owns the modem itself: bring-up, recovery
// from cellular drops, and the sockets.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <Client.h>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class Cellular {
public:
  // RAII guard for the SIM7080 AT bus. Every call site that touches
  // Serial1 / TinyGSM must instantiate one of these at function entry.
  // A FreeRTOS recursive mutex serializes the socket clients, the loop()
  // health probe and shell AT passthrough.
  //
  // Recursive: nested helpers can re-acquire without self-deadlock.
  //
  // Default 30 s acquire window: a wedged AT bus surfaces as a returnable
  // failure rather than an infinite block. Pass 0 for a non-blocking
  // try-acquire (socket available() polls step aside that way).
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
  // Does NOT register on the network. Lets the GNSS path bring the modem
  // up without taking over network duty (the SIM7080 GNSS receiver
  // shares the modem core; reading it requires the modem be powered,
  // but not registered).
  // Idempotent: returns immediately if the modem is already AT-responsive.
  // Caller-safe from any task; takes the AT-bus mutex internally.
  // out: true if modem AT-OK after the call.
  static bool powerOn();

  // Result of one Cellular::tickActivation() call.
  enum class ActStatus {
    PENDING,  // bring-up still walking its phases - poll again next tick
    DONE,     // modem registered + bearer up; sockets usable
    FAILED,   // hard failure (modem dead / SIM absent / activation timeout)
  };

  // Incremental cellular bring-up. Replaces the old blocking begin():
  // each call advances at most one phase of the modem activation state
  // machine (power-on -> SIM -> radio config -> registration -> bearer)
  // and returns fast, so every other module loop keeps ticking
  // between phases. CellularModule polls this each loop while ACTIVATING.
  //
  // Registration is polled one status read per call (~1 Hz). A
  // configurable deadline (cellular.activation_timeout_ms, default
  // 30 min) bounds the whole cycle: on expiry the modem is hardware-
  // reset and FAILED returned, so the device drops back to WiFi-watching
  // standby instead of looping a wedged modem forever.
  //
  // Idempotent: returns DONE immediately if cellular is already up
  // (re-entry after a yield). Resets the internal phase to the top on
  // DONE or FAILED so the next ACTIVATING cycle walks from scratch.
  static ActStatus tickActivation();

  // Cellular-only recovery loop: re-register and re-open the bearer on a
  // network drop. Does NOT touch WiFi; CellularModule owns the
  // WiFi-vs-cellular selection.
  static void loop();

  // True when activation finished and the last health probe saw
  // registration and an active bearer.
  static bool connected();

  // True once activation reached DONE and until a hard reset or
  // dataLinkDown() clears it.
  static bool activated() { return _started; }

  // Plain TCP sockets on the modem. Each is an Arduino Client that takes
  // the AT bus per exchange. They live for the whole program.
  static constexpr uint8_t TCP_SLOTS      = 2;
  static constexpr uint8_t TCP_SLOT_MQTT  = 0;
  static constexpr uint8_t TCP_SLOT_HTTPS = 1;
  // in: slot index. out: socket client, nullptr for an unknown slot.
  static Client* tcpClient(uint8_t slot);

  // Last cached signal quality from AT+CSQ. 0..31 valid, 99 = unknown.
  // Updated by Cellular::loop() when started; returns 99 before first sample.
  static int  getSignalQuality();

  // Send a raw AT command to the SIM7080 modem and stream the response
  // back to `emit`. Bypasses TinyGSM so it can be used at any state, even
  // when the network connect path is mid-flight. Pass `cmd` without the
  // "AT" prefix (e.g. "+CSQ", "+COPS=?"). `timeoutMs` should be generous
  // for slow commands like operator scan (>= 120s).
  static void atPassthrough(const char* cmd, uint32_t timeoutMs,
                            std::function<void(const char*)> emit);

  // True once the modem is powered and AT-responsive (after powerOn() or
  // begin()). Required precondition for any GNSS use - the SIM7080 GNSS
  // receiver shares the same modem core. Registration does not need to
  // be up for GNSS.
  static bool isModemAlive();

  // Hardware-reset the modem: PowerManager::resetModem() (deeper DC3 +
  // BLDO2 1 s cycle) followed by PWRKEY pulse and AT-probe wait. Last-
  // resort recovery for a wedged modem. After this returns, _modemAlive /
  // _started are reset and every socket is marked closed;
  // CellularModule::loop() will re-walk activation on the next tick.
  // Returns true if AT comes back within budget.
  static bool hardReset();

  // HTTPS GET over TlsClient on socket slot TCP_SLOT_HTTPS. Used by the
  // cellular OTA path and net.http when WiFi is down. TLS runs on the
  // ESP32, so the server chain and host name are checked exactly as on
  // WiFi.
  //
  // Pre-conditions: connected(). `caPem` is the PEM bundle the server
  // must chain to; nullptr skips verification, so only pass it when the
  // caller's own insecure opt-in allows that.
  //
  // `writeCallback` is invoked with each chunk of the response BODY
  // (status line + headers are parsed away). Returning false from the
  // callback aborts the transfer; the socket is still cleanly closed.
  //
  // in:  host         hostname without scheme (e.g. "ota.thesada.app")
  //      path         path with leading slash (e.g. "/latest/foo.json")
  //      port         TCP port (443 for HTTPS)
  //      caPem        PEM bundle to verify the server, nullptr = insecure
  //      writeCallback per-chunk body delivery
  //      rangeStart, rangeEndInclusive  optional Range request bounds.
  //                   When rangeEndInclusive > 0, the helper adds
  //                   `Range: bytes=<start>-<end>` to the GET and
  //                   expects a 206 response. Used by the cellular OTA
  //                   path so each chunk is its own short-lived socket.
  // out: httpStatus   HTTP response status code on the wire, or 0 on
  //                   transport failure (connect / TLS handshake / EOF
  //                   before status line)
  // returns: true if the request completed and (when a Content-Length
  //          was advertised) at least that many body bytes reached the
  //          callback. false on transport failure, callback abort, or
  //          short body.
  static bool httpsGet(const char* host, const char* path, uint16_t port,
                       const char* caPem,
                       std::function<bool(const uint8_t*, size_t)> writeCallback,
                       int* httpStatus,
                       size_t rangeStart = 0,
                       size_t rangeEndInclusive = 0);

  // GNSS fix acquisition. The SIM7080G time-shares its radio
  // between LTE and GNSS - while CGNSPWR=1 the LTE data path is suspended,
  // so socket writes issued during that window stall. The
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

  // Close the sockets and the data context (CNACT slot 0) while leaving
  // the modem powered and network-registered. Drops _started so
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
  // Synchronous network bring-up (radio config + blocking registration
  // poll + bearer). Used by the steady-state recovery path in
  // Cellular::loop(). The incremental tickActivation() uses the three helpers below directly
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
  static bool isRegistered();
  static void sampleSignalQuality();

  // Line-oriented AT helper over Serial1. Straggler URCs are handed to
  // TinyGSM first so socket state stays right.
  //
  // Caller must hold the AT-bus mutex (ATGuard).
  //
  // in:  cmd          AT command without leading "AT" or trailing CRLF
  //                   (e.g. "+CSQ", "+CNACT?")
  //      expect       success terminator the caller is waiting for
  //                   (typically "OK")
  //      timeoutMs    upper bound on response window
  //      lineCallback per non-terminator line; nullptr ok
  // out: int           1 on expect match, 0 on timeout, -1 on ERROR
  static int  cellAT(const char* cmd, const char* expect, uint32_t timeoutMs,
                     std::function<void(const char*)> lineCallback);

  static bool isGprsConnectedRaw();

  static bool     _modemAlive;     // PMU + wakeModem ok (powerOn complete)
  static bool     _started;        // tickActivation() reached DONE (bearer up)
  static int      _signalQuality;
  static uint32_t _lastSignalSample;

  // AT-bus mutex. Lazy-initialised (xSemaphoreCreateRecursiveMutex)
  // on first ATGuard construction so static init order is irrelevant.
  static SemaphoreHandle_t _atMutex;
  static void atMutexInit();
};
