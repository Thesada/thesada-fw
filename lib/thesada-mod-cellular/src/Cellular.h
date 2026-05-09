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

  // Initialise PMU, wake modem, check SIM, write CA cert, register, MQTT connect.
  // Calls powerOn() first; powerOn is idempotent if the modem is already up
  // (e.g. GNSS warmed it earlier). Returns immediately if MQTT is already
  // connected. Side-effect: leaves the publish gate open on success.
  static void begin();

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
  static bool publish(const char* topic, const char* payload);

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

private:
  static void initPMU();
  static bool wakeModem();
  static bool writeCACert();
  static bool networkConnect();
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
  static void routeSmsubLine(char* line);
  static bool isGprsConnectedRaw();

  static bool     _modemAlive;     // PMU + wakeModem ok (powerOn complete)
  static bool     _started;        // begin() fully completed (MQTT up)
  static bool     _mqttConnected;
  static bool     _publishGate;
  static bool     _hasCACert;
  static int      _signalQuality;
  static uint32_t _lastSignalSample;

  // AT-bus mutex. Lazy-initialised (xSemaphoreCreateRecursiveMutex)
  // on first ATGuard construction so static init order is irrelevant.
  static SemaphoreHandle_t _atMutex;
  static void atMutexInit();
};
