// thesada-fw - Cellular.cpp
// SIM7080G Cat-M1 with modem-native MQTT over TLS (AT+SMCONF/SMCONN/SMPUB).
// SPDX-License-Identifier: GPL-3.0-only

#include <thesada_config.h>
#ifdef ENABLE_CELLULAR

#include "Cellular.h"
#include <Config.h>
#include <Log.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>
#include <Shell.h>
#include <MQTTClient.h>
#include <sys/time.h>
#include <time.h>
#include <memory>
#include <new>
#include <mbedtls/platform_util.h>

// ── TinyGSM ─────────────────────────────────────────────────────────────────
// TINY_GSM_MODEM_SIM7080 and TINY_GSM_RX_BUFFER=1024 set via build_flags.
#include <TinyGsmClient.h>

// PMU rails go through PowerManager's persistent _pmu (see initPMU).
#include <PowerManager.h>

static const char* TAG = "Cellular";

// ── Board pins (LILYGO T-SIM7080-S3) ─────────────────────────────────────────
#ifdef BOARD_LILYGO_T_SIM7080_S3
  static constexpr int PIN_MODEM_RXD = 4;
  static constexpr int PIN_MODEM_TXD = 5;
  static constexpr int PIN_MODEM_PWR = 41;
  static constexpr int PIN_MODEM_DTR = 42;
#else
  #error "No board defined - set BOARD_LILYGO_T_SIM7080_S3 in config.h"
#endif


// ── Module state ─────────────────────────────────────────────────────────────
bool              Cellular::_modemAlive    = false;
bool              Cellular::_started        = false;
bool              Cellular::_mqttConnected  = false;
bool              Cellular::_publishGate    = false;
bool              Cellular::_hasCACert      = false;
bool              Cellular::_hasClientCertOnModem = false;
int               Cellular::_signalQuality  = 99;
uint32_t          Cellular::_lastSignalSample = 0;
uint32_t          Cellular::_lastSmpubMs    = 0;
SemaphoreHandle_t Cellular::_atMutex        = nullptr;

// Lazy init of the recursive AT-bus mutex. Called from every ATGuard
// constructor so static init order across translation units does not
// matter; first ATGuard wins, subsequent constructions skip the create.
void Cellular::atMutexInit() {
  if (_atMutex == nullptr) {
    _atMutex = xSemaphoreCreateRecursiveMutex();
  }
}

Cellular::ATGuard::ATGuard(uint32_t timeoutMs) : _held(false) {
  atMutexInit();
  if (_atMutex == nullptr) return;  // OOM at create time - degrade open
  TickType_t to = (timeoutMs == 0) ? 0 : pdMS_TO_TICKS(timeoutMs);
  if (xSemaphoreTakeRecursive(_atMutex, to) == pdTRUE) {
    _held = true;
  }
}
Cellular::ATGuard::~ATGuard() {
  if (_held) xSemaphoreGiveRecursive(_atMutex);
}

// Temporarily release the bus, chunked-sleep with shell + WDT pumps,
// then re-acquire before returning. Used by mqttBackoffWait so a long
// MQTT retry backoff inside the Cellular::loop() recovery path does not
// starve the main loop (Shell, console, cell.at debug commands, URC
// pump). See header for caller contract: no AT commands while paused.
void Cellular::ATGuard::pause(uint32_t pauseMs) {
  bool wasHeld = _held;
  if (wasHeld) {
    xSemaphoreGiveRecursive(_atMutex);
    _held = false;
  }

  uint32_t remaining = pauseMs;
  while (remaining) {
    uint32_t step = remaining > 100UL ? 100UL : remaining;
    delay(step);
    esp_task_wdt_reset();
    // Service the console so the user can issue cell.at / cell.cert.*
    // / restart while the cellular bring-up retry is paused. pumpConsole
    // is non-blocking; if there is no input, it returns immediately.
    Shell::pumpConsole();
    remaining -= step;
  }

  if (wasHeld) {
    // Re-acquire with a 60 s budget. If another holder appeared during
    // the pause and will not release, leave _held=false so the caller
    // sees ok()==false and bails the retry loop instead of issuing AT
    // commands without the bus.
    TickType_t to = pdMS_TO_TICKS(60000UL);
    if (xSemaphoreTakeRecursive(_atMutex, to) == pdTRUE) {
      _held = true;
    } else {
      Log::warn("Cellular", "ATGuard pause: re-acquire timed out");
    }
  }
}

// Serial port for TinyGSM
static TinyGsm modem(Serial1);

// Sample AT+CSQ at most every 30 s to keep AT bus clean.
static constexpr uint32_t SIGNAL_SAMPLE_MS = 30UL * 1000UL;

// Minimum spacing between Cellular::publish() calls. Defensive guard
// against the burst-wedge pattern: ~10 SMPUBs in the first ~10 s of
// a fresh cellular MQTT session can wedge SIM7080G URC routing even
// with cellATWrite on every publish, so spread bursts to <= 1 Hz.
static constexpr uint32_t SMPUB_MIN_SPACING_MS = 1000UL;

// MQTT-active health probe cadence. Per-tick TinyGSM AT calls would
// drain Serial1 of any +SMSUB: URC arriving between loop iterations,
// so during MQTT-active phase we only run isRegistered + isGprsConnected
// + mqttIsConnected once every HEALTH_PROBE_MS. The ATGuard mutex held
// across the probe blocks pumpInbound for that one tick.
static constexpr uint32_t HEALTH_PROBE_MS = 60UL * 1000UL;
static uint32_t           s_lastHealthMs   = 0;

// TinyGSM waitResponse blocks the task for the full timeout without
// feeding the 5 s task watchdog. This wrapper splits any long wait into
// 1.5 s chunks with esp_task_wdt_reset() between, so SIM7080 commands
// that legitimately need 10-30 s (CFSWFILE, SMCONN, registration) do
// not trip TWDT on slow cycles. Returns the same codes as waitResponse:
// 1 on OK match, >1 on alternate matches, 0 on timeout.
template <typename... Args>
static int waitChunked(uint32_t totalMs, Args... args) {
  uint32_t end = millis() + totalMs;
  int res = 0;
  while (millis() < end) {
    uint32_t left  = end - millis();
    uint32_t chunk = left > 1500 ? 1500 : left;
    res = modem.waitResponse(chunk, args...);
    if (res != 0) return res;
    esp_task_wdt_reset();
  }
  return 0;
}

// MQTT retry backoff state. Reset on a successful connect cycle, doubled
// on each failure up to MQTT_RETRY_MAX_MS so the modem and broker are not
// hammered when the cellular session is wedged.
static constexpr uint32_t MQTT_RETRY_INIT_MS = 10UL * 1000UL;
static constexpr uint32_t MQTT_RETRY_MAX_MS  = 5UL * 60UL * 1000UL;
static uint32_t           s_mqttRetryMs      = MQTT_RETRY_INIT_MS;

// Sleep current MQTT backoff window, then double it (capped at
// MQTT_RETRY_MAX_MS), feeding the task watchdog every 500 ms so a
// long backoff does not trip TWDT.
//
// If `guard` is non-null, it is paused for the backoff so the AT bus
// is released and the shell console pumps for cell.at / cell.cert.*
// debug commands. The Cellular::loop() recovery path passes its own
// guard; callers without a guard get a plain delay loop (no console
// pump). The incremental tickActivation() path does not use this -
// it schedules a non-blocking retry timestamp instead.
//
// in:  guard  optional ATGuard to pause during the sleep, or nullptr
// out: none (uses module-static s_mqttRetryMs)
static void mqttBackoffWait(Cellular::ATGuard* guard = nullptr) {
  uint32_t ms = s_mqttRetryMs;
  char msg[64];
  snprintf(msg, sizeof(msg), "Retry MQTT in %lu s", (unsigned long)(ms / 1000UL));
  Log::warn(TAG, msg);
  if (guard) {
    guard->pause(ms);
  } else {
    uint32_t remaining = ms;
    while (remaining) {
      uint32_t step = remaining > 500UL ? 500UL : remaining;
      delay(step);
      esp_task_wdt_reset();
      remaining -= step;
    }
  }
  uint32_t next = ms * 2UL;
  s_mqttRetryMs = (next > MQTT_RETRY_MAX_MS) ? MQTT_RETRY_MAX_MS : next;
}

// Reset MQTT retry backoff to the initial window after a successful
// connect cycle, so the next failure starts at the short delay again.
static inline void mqttBackoffReset() {
  s_mqttRetryMs = MQTT_RETRY_INIT_MS;
}

// Hardware-reset the modem by dropping DC3 for 200 ms via the PMU,
// then re-pulsing PWRKEY and waiting for AT. Replaces the previous
// `+CFUN=1,1` recovery path: a forced reboot via AT can leave the
// SIM7080's URC routing in a degraded state that survives session
// reconnects. Hard-cycling Vcc clears every internal modem state.
//
// CMEE=2 has to be re-applied after reset since it does not survive.
//
// in:  none
// out: bool   true if AT comes back within REBOOT_BUDGET_MS
static bool modemSoftReset() {
  static constexpr uint32_t REBOOT_BUDGET_MS = 60UL * 1000UL;
  Log::warn(TAG, "Hardware-resetting modem (DC3 power-cycle)");
  if (!PowerManager::resetModem()) {
    Log::error(TAG, "PMU resetModem failed");
    return false;
  }

  // PWRKEY pulse to bring the modem out of POWER_OFF (datasheet >=500ms low).
  pinMode(PIN_MODEM_PWR, OUTPUT);
  digitalWrite(PIN_MODEM_PWR, LOW);  delay(100);
  digitalWrite(PIN_MODEM_PWR, HIGH); delay(1000);
  digitalWrite(PIN_MODEM_PWR, LOW);

  uint32_t deadline = millis() + REBOOT_BUDGET_MS;
  while (millis() < deadline) {
    delay(1000);
    esp_task_wdt_reset();
    if (modem.testAT(1000UL)) {
      Log::info(TAG, "Modem back after hardware reset");
      modem.sendAT("+CMEE=2");
      modem.waitResponse(2000UL);
      return true;
    }
  }
  Log::error(TAG, "Modem hardware-reset timeout");
  return false;
}

// Drop the modem-side cached client cert (next mqttConnect re-decides
// based on current NVS state) AND tear down any live SMCONN so the
// next reconnect cycle uses the new auth mode. Wired to MQTTClient's
// cert-cleared hook from begin(); also safe to call directly. No-op
// when cellular MQTT was never up. Held briefly on the AT bus.
void Cellular::invalidateClientCert() {
  _hasClientCertOnModem = false;
  if (!_modemAlive) return;

  ATGuard g(5000UL);
  if (!g.ok()) {
    // The bus is busy - the next health probe / reconnect will pick
    // up _hasClientCertOnModem=false and re-decide; the live session
    // continues until then. Acceptable for a runtime cert.clear.
    Log::warn(TAG, "invalidateClientCert: AT bus busy - deferring SMDISC");
    return;
  }
  if (_mqttConnected) {
    Log::info(TAG, "cert.clear: dropping modem MQTT session");
    modem.sendAT("+SMDISC");
    modem.waitResponse(5000UL);
    _mqttConnected = false;
  }
}

// Public wrapper for the hardware reset. Drops state flags so the next
// CellularModule::loop tick re-walks activation (networkConnect +
// mqttConnect + smsubAll). Caller can be Shell, watchdog, or anything
// that needs to recover a wedged modem.
bool Cellular::hardReset() {
  ATGuard g(120000UL);
  if (!g.ok()) {
    Log::error(TAG, "hardReset: AT bus stuck");
    return false;
  }
  bool ok = modemSoftReset();
  _modemAlive    = ok;
  _started       = false;
  _mqttConnected = false;
  // Modem FS does not survive a DC3 power cycle - any prior cert
  // upload is gone, force a re-upload on the next mqttConnect.
  _hasClientCertOnModem = false;
  return ok;
}

// Slurp pending bytes from the modem stream for a short window and
// return them as a one-line String (CR/LF replaced with " | "). Used
// to capture +CME ERROR / SMSTATE chatter after an AT failure when
// waitChunked returns 0, so the log line is actionable instead of a
// bare "MQTT connect failed".
//
// in:  windowMs  how long to keep slurping bytes
// out: String    sanitized tail (may be empty)
static String drainModemTail(uint32_t windowMs = 300UL) {
  uint32_t t0 = millis();
  String buf;
  while (millis() - t0 < windowMs) {
    while (modem.stream.available()) buf += (char)modem.stream.read();
    delay(10);
    esp_task_wdt_reset();
  }
  buf.replace("\r", " ");
  buf.replace("\n", " | ");
  buf.trim();
  return buf;
}

// ---------------------------------------------------------------------------

// Configure AXP2101 PMU power rails for the modem via PowerManager's
// persistent static _pmu instance. A previous version of this function
// instantiated a stack-local XPowersPMU; on return its destructor called
// XPowersAXP2101::deinit() -> __wire->end() which nulled Wire1's TX
// buffer, crashing the next battery poll with "NULL TX buffer pointer".
void Cellular::initPMU() {
  if (!PowerManager::setModemRails()) {
    Log::error(TAG, "PMU not available - cannot bring up modem rails");
    return;
  }
  Log::info(TAG, "PMU ready");
}

// ---------------------------------------------------------------------------

// Start serial and pulse PWRKEY until the modem responds to AT.
// Bounded by WAKE_BUDGET_MS so a wedged or unpowered modem fails cleanly
// instead of hanging the loop task long enough to trip the TWDT.
// Returns true on AT-OK, false on timeout.
bool Cellular::wakeModem() {
  Serial1.begin(115200, SERIAL_8N1, PIN_MODEM_RXD, PIN_MODEM_TXD);
  pinMode(PIN_MODEM_PWR, OUTPUT);
  pinMode(PIN_MODEM_DTR, OUTPUT);

  static constexpr uint32_t WAKE_BUDGET_MS = 60UL * 1000UL;
  uint32_t start = millis();
  int retry = 0;
  while (!modem.testAT(1000)) {
    esp_task_wdt_reset();
    if (millis() - start > WAKE_BUDGET_MS) {
      Log::error(TAG, "Modem AT timeout");
      return false;
    }
    if (++retry > 6) {
      Log::info(TAG, "PWRKEY pulse");
      digitalWrite(PIN_MODEM_PWR, LOW);  delay(100); esp_task_wdt_reset();
      digitalWrite(PIN_MODEM_PWR, HIGH); delay(1000); esp_task_wdt_reset();
      digitalWrite(PIN_MODEM_PWR, LOW);
      retry = 0;
    }
  }
  Log::info(TAG, "Modem alive");

  // Verbose CME ERROR (text) so failure tail logs are actionable. Mode 2
  // returns the textual cause string after every CME ERROR.
  modem.sendAT("+CMEE=2");
  modem.waitResponse(2000UL);
  return true;
}

// ---------------------------------------------------------------------------

// Upload CA certificate from LittleFS to the modem for TLS verification
bool Cellular::writeCACert() {
  // Load CA cert from LittleFS /ca.crt. Required for TLS.
  // If not present, skip cert write - modem TLS will connect without verification.
  String certBuf;
  if (LittleFS.exists("/ca.crt")) {
    File cf = LittleFS.open("/ca.crt", "r");
    if (cf) { certBuf = cf.readString(); cf.close(); Log::info(TAG, "CA cert loaded from /ca.crt"); }
  }
  if (certBuf.isEmpty()) {
    Log::warn(TAG, "No /ca.crt - skipping CA cert write, TLS without verification");
    _hasCACert = false;
    return true;
  }
  const char* cert    = certBuf.c_str();
  size_t      certLen = certBuf.length();

  // Terminate any previous FS session, then open.
  modem.sendAT("+CFSTERM");
  modem.waitResponse();
  modem.sendAT("+CFSINIT");
  if (modem.waitResponse() != 1) {
    Log::error(TAG, "CFSINIT failed");
    return false;
  }

  size_t total   = certLen;
  size_t written = 0;
  while (total > 0) {
    size_t chunk = total > 10000 ? 10000 : total;
    modem.sendAT("+CFSWFILE=", 3, ",\"server-ca.crt\",",
                 (written > 0 ? 1 : 0), ",", chunk, ",10000");
    waitChunked(30000UL, "DOWNLOAD");
    modem.stream.write(cert + written, chunk);
    if (waitChunked(30000UL) == 1) {
      written += chunk;
      total   -= chunk;
    } else {
      Log::warn(TAG, "CA chunk write failed, retrying");
      delay(1000);
    }
  }

  modem.sendAT("+CFSTERM");
  modem.waitResponse();
  _hasCACert = true;
  Log::info(TAG, "CA cert written to modem");
  return true;
}

// ---------------------------------------------------------------------------

// Chunked CFSWFILE upload helper. Same flow as writeCACert: open a
// session via CFSINIT, walk the buffer in <= 10 KB chunks via CFSWFILE
// (truncate write on chunk 0, append on subsequent), close with CFSTERM.
// Caller owns the AT bus mutex.
//
// in:  filename  modem-side file name (in OEM partition, index 3)
//      data      pointer to PEM bytes
//      length    byte count
// out: bool      true on success
static bool writeModemFile(TinyGsm& modem, const char* filename,
                           const uint8_t* data, size_t length) {
  modem.sendAT("+CFSTERM");
  modem.waitResponse();
  modem.sendAT("+CFSINIT");
  if (modem.waitResponse() != 1) return false;

  size_t remaining = length;
  size_t written   = 0;
  while (remaining > 0) {
    size_t chunk = remaining > 10000 ? 10000 : remaining;
    modem.sendAT("+CFSWFILE=", 3, ",\"", filename, "\",",
                 (written > 0 ? 1 : 0), ",", chunk, ",10000");
    waitChunked(30000UL, "DOWNLOAD");
    modem.stream.write(data + written, chunk);
    if (waitChunked(30000UL) == 1) {
      written   += chunk;
      remaining -= chunk;
    } else {
      delay(1000);
    }
  }

  modem.sendAT("+CFSTERM");
  modem.waitResponse();
  return true;
}

// Upload the NVS-stored client cert and private key as TWO separate
// PEM files on the modem FS - `client.crt` for the certificate,
// `client.key` for the private key. SIM7080G fw 1951B17 wants
// CSSLCFG type 1 with the 4-arg form (cert filename + key filename
// as separate args), so the two-file layout matches what mqttConnect's
// CSSLCFG + SMSSL sequence references.
//
// Earlier impl concatenated cert+key into one PEM and used SMSSL=2,
// which silently fails the TLS handshake on this fw rev (broker logs
// `unexpected eof while reading`, modem closes mid-handshake). Pattern
// here mirrors the LilyGo SIMCom AT samples (AWS IoT + ATT mosquitto)
// that work on the same modem family.
//
// in:  none (NVS via MQTTClient::loadClientCert; AT bus via outer ATGuard)
// out: bool  true on full upload + CFSTERM ack for both files; false if
//             NVS empty, alloc fails, or any AT step errors out
bool Cellular::writeClientCert() {
  if (!MQTTClient::hasClientCert()) return false;

  const size_t maxLen = MQTTClient::CERT_MAX_LEN;
  char* certBuf = (char*)malloc(maxLen);
  char* keyBuf  = (char*)malloc(maxLen);

  // Zero + free helper - the private key half of the pair must never linger
  // in heap past use. mbedtls_platform_zeroize cannot be optimised away by
  // the compiler, unlike memset which is legal to skip on a buffer that is
  // about to be free'd.
  auto wipe_and_free = [maxLen](char*& cert, char*& key) {
    if (cert) { mbedtls_platform_zeroize(cert, maxLen); free(cert); cert = nullptr; }
    if (key)  { mbedtls_platform_zeroize(key,  maxLen); free(key);  key  = nullptr; }
  };

  if (!certBuf || !keyBuf) {
    Log::error(TAG, "writeClientCert: heap alloc failed");
    wipe_and_free(certBuf, keyBuf);
    return false;
  }

  if (!MQTTClient::loadClientCert(certBuf, keyBuf, maxLen)) {
    Log::warn(TAG, "writeClientCert: NVS load failed");
    wipe_and_free(certBuf, keyBuf);
    return false;
  }
  size_t certLen = strlen(certBuf);
  size_t keyLen  = strlen(keyBuf);
  if (certLen == 0 || keyLen == 0) {
    Log::warn(TAG, "writeClientCert: empty cert or key");
    wipe_and_free(certBuf, keyBuf);
    return false;
  }

  bool ok = writeModemFile(modem, "client.crt",
                           (const uint8_t*)certBuf, certLen);
  if (ok) {
    ok = writeModemFile(modem, "client.key",
                        (const uint8_t*)keyBuf, keyLen);
  }

  wipe_and_free(certBuf, keyBuf);
  if (!ok) {
    Log::error(TAG, "writeClientCert: upload failed");
    return false;
  }

  char msg[80];
  snprintf(msg, sizeof(msg),
           "Client cert+key written to modem (cert %u B, key %u B)",
           (unsigned)certLen, (unsigned)keyLen);
  Log::info(TAG, msg);
  return true;
}

// ---------------------------------------------------------------------------

// Radio configuration block of the network bring-up: CFUN power-cycle,
// network/preferred-mode select, APN (CGDCONT/CNCFG), CEREG/CGREG URC
// enable, then a configurable RF-settle delay before registration is
// polled. Caller owns the AT bus. Shared by the synchronous
// networkConnect() and the incremental tickActivation() RADIO_CFG phase.
//
// The CFUN waits + RF settle are chunked so the task watchdog stays fed;
// the whole sequence is one ~40 s burst that otherwise blocks > TWDT.
void Cellular::radioConfigure() {
  JsonObject  cfg        = Config::get();
  const char* apn        = cfg["cellular"]["apn"]          | "OSC";
  uint32_t    rfSettleMs = cfg["cellular"]["rf_settle_ms"] | 15000;

  Log::info(TAG, "Configuring radio...");
  modem.sendAT("+CFUN=0");
  waitChunked(20000UL);
  delay(500);

  modem.setNetworkMode(2);   // AUTO
  modem.setPreferredMode(3); // LTE-M + NB-IoT

  modem.sendAT("+CGDCONT=1,\"IP\",\"", apn, "\"");
  modem.waitResponse();
  modem.sendAT("+CNCFG=0,1,\"", apn, "\"");
  modem.waitResponse();

  modem.sendAT("+CGREG=2");
  modem.waitResponse();
  modem.sendAT("+CEREG=2");
  modem.waitResponse();

  modem.sendAT("+CFUN=1");
  waitChunked(20000UL);

  // Critical: let LTE-M radio settle before polling registration.
  char msg[64];
  snprintf(msg, sizeof(msg), "RF settle %lu ms...", (unsigned long)rfSettleMs);
  Log::info(TAG, msg);
  uint32_t remaining = rfSettleMs;
  while (remaining) {
    uint32_t step = remaining > 500UL ? 500UL : remaining;
    delay(step);
    esp_task_wdt_reset();
    remaining -= step;
  }
}

// ---------------------------------------------------------------------------

// One network-registration status read for the bring-up paths. Returns
// 1 on HOME/ROAMING, 0 while still searching, -1 on REG_DENIED. The
// caller owns the registration timeout. THESADA_CELL_DEBUG builds also
// dump CPSI/CEREG/CGREG at most every 5 s while still searching.
int Cellular::pollRegistration() {
  esp_task_wdt_reset();
  SIM70xxRegStatus s = modem.getRegistrationStatus();
  if (s == REG_OK_HOME)    { Log::info(TAG, "Registered - HOME");    return 1; }
  if (s == REG_OK_ROAMING) { Log::info(TAG, "Registered - ROAMING"); return 1; }
  if (s == REG_DENIED)     { Log::error(TAG, "Registration DENIED"); return -1; }
#ifdef THESADA_CELL_DEBUG
  static uint32_t lastDiag = 0;
  if (millis() - lastDiag > 5000UL) {
    lastDiag = millis();
    char dbg[160];
    snprintf(dbg, sizeof(dbg), "diag: regStatus=%d CSQ=%d",
             (int)s, modem.getSignalQuality());
    Log::info(TAG, dbg);
    auto dumpAT = [](const char* cmd) {
      Serial1.printf("AT%s\r\n", cmd);
      uint32_t t0 = millis();
      String buf;
      while (millis() - t0 < 1500UL) {
        while (Serial1.available()) buf += (char)Serial1.read();
        delay(20);
      }
      buf.replace("\r", " ");
      buf.replace("\n", " | ");
      char line[256];
      snprintf(line, sizeof(line), "AT%s -> %s", cmd, buf.c_str());
      Log::info(TAG, line);
    };
    dumpAT("+CPSI?");
    dumpAT("+CEREG?");
    dumpAT("+CGREG?");
  }
#endif
  return 0;
}

// ---------------------------------------------------------------------------

// Activate the PDP data context (CNACT slot 0) after registration.
// Falls back to TinyGSM gprsConnect() if the direct CNACT does not
// confirm. Caller owns the AT bus. Returns true once isGprsConnected().
bool Cellular::activateBearer() {
  JsonObject  cfg = Config::get();
  const char* apn = cfg["cellular"]["apn"] | "OSC";

  Log::info(TAG, "Activating bearer...");
  if (!modem.isGprsConnected()) {
    modem.sendAT("+CNACT=0,1");
    if (waitChunked(30000UL) != 1) {
      modem.gprsConnect(apn, "", "");
    }
  }

  if (!modem.isGprsConnected()) {
    Log::error(TAG, "Bearer activation failed");
    return false;
  }

  char ipMsg[64];
  snprintf(ipMsg, sizeof(ipMsg), "IP: %s", modem.localIP().toString().c_str());
  Log::info(TAG, ipMsg);
  return true;
}

// ---------------------------------------------------------------------------

// Configure radio, register on LTE-M network, and activate data bearer.
// Synchronous (blocking registration poll) - used by the steady-state
// recovery path in Cellular::loop() and the modem-wedge path in
// mqttConnect(). The incremental CellularModule activation walks
// radioConfigure() / pollRegistration() / activateBearer() through
// tickActivation() instead, so it can yield between phases.
bool Cellular::networkConnect() {
  radioConfigure();

  Log::info(TAG, "Waiting for registration...");
  JsonObject cfg        = Config::get();
  uint32_t   regTimeout = cfg["cellular"]["reg_timeout_ms"] | 180000;
  uint32_t   start      = millis();
  for (;;) {
    Shell::pumpConsole();
    int r = pollRegistration();
    if (r > 0) break;
    if (r < 0) return false;  // REG_DENIED
    delay(1000);
    if (millis() - start > regTimeout) {
      Log::error(TAG, "Registration timeout");
      return false;
    }
  }

  return activateBearer();
}

// ---------------------------------------------------------------------------

// Configure and connect modem-native MQTT over TLS
bool Cellular::mqttConnect() {
  JsonObject  cfg      = Config::get();
  const char* broker   = cfg["mqtt"]["broker"]   | "mqtt.example.com";
  int         port     = cfg["mqtt"]["port"]      | 8883;
  const char* user     = cfg["mqtt"]["user"]      | "";
  const char* password = cfg["mqtt"]["password"]  | "";
  const char* devName  = cfg["device"]["name"]    | "thesada-node";

  Log::info(TAG, "Configuring MQTT...");

  // Strong teardown: SMDISC alone is not enough after a warm SMCONN
  // failure - the SIM7080 keeps the URL slot half-locked and the next
  // SMCONF "URL" returns ERROR. Probe SMSTATE; if the modem still
  // thinks a session is up, recycle data context 0 (CNACT=0,0 then
  // CNACT=0,1) before reconfiguring.
  modem.sendAT("+SMDISC");
  modem.waitResponse(5000UL);

  int smState = -1;
  modem.sendAT("+SMSTATE?");
  if (modem.waitResponse(2000UL, "+SMSTATE: ") == 1) {
    smState = modem.stream.parseInt();
    modem.waitResponse();
  }
  if (smState == -1) {
    // Modem ignored SMSTATE? - AT bus wedged. Bench data shows bearer
    // cycle here does nothing because CNACT also gets ignored; only
    // a CFUN=1,1 soft reset clears it. After reboot we have to redo
    // CA upload and network registration before retrying SMCONF.
    Log::warn(TAG, "SMSTATE no response - modem wedged");
    if (!modemSoftReset()) return false;
    // Modem FS cleared by the power cycle; next CFSWFILE writes the
    // CA + (if mTLS) the client cert+key from a clean slate.
    _hasCACert = false;
    _hasClientCertOnModem = false;
    writeCACert();
    if (!networkConnect()) return false;
  } else if (smState != 0) {
    char dbg[64];
    snprintf(dbg, sizeof(dbg), "SMSTATE=%d not idle, recycling bearer", smState);
    Log::warn(TAG, dbg);
    modem.sendAT("+CNACT=0,0");
    modem.waitResponse(5000UL);
    delay(500);
    modem.sendAT("+CNACT=0,1");
    if (waitChunked(20000UL) != 1) {
      Log::warn(TAG, "Bearer reactivation slow - continuing");
    }
  }

  modem.sendAT("+SMCONF=\"URL\",", broker, ",", port);
  if (modem.waitResponse() != 1) {
    String tail = drainModemTail();
    char line[256];
    snprintf(line, sizeof(line), "MQTT URL failed: %s", tail.c_str());
    Log::error(TAG, line);
    return false;
  }

  // KEEPTIME=15 forces MQTT PINGREQ every 15 s. Default 60 s on
  // some IoT cellular carriers leaves a long drift window where
  // inbound MQTT forwarding silently stops while outbound SMPUB
  // still works - looks like asymmetric NAT timeout on the broker->
  // modem direction. 15 s pings keep that path warm. Bench-observed
  // on multiple roaming partners.
  modem.sendAT("+SMCONF=\"KEEPTIME\",15");
  modem.waitResponse();

  modem.sendAT("+SMCONF=\"CLEANSS\",1");
  modem.waitResponse();

  modem.sendAT("+SMCONF=\"CLIENTID\",\"", devName, "\"");
  if (modem.waitResponse() != 1) { Log::error(TAG, "MQTT ClientID failed"); return false; }

  // mTLS detection: NVS holds a client cert AND we already wrote (or
  // can now write) the matching cert+key onto the modem FS. Skip the
  // upload if the cache flag says it is already there from a prior
  // session - the modem FS persists across SMDISC, just not across
  // a hardware power cycle (handled by hardReset / modemSoftReset).
  bool wantMTLS = MQTTClient::hasClientCert() && _hasCACert;
  if (wantMTLS && !_hasClientCertOnModem) {
    if (writeClientCert()) {
      _hasClientCertOnModem = true;
    } else {
      Log::warn(TAG, "Client cert upload failed - falling back to user/pass");
      wantMTLS = false;
    }
  }

  if (!wantMTLS) {
    modem.sendAT("+SMCONF=\"USERNAME\",\"", user, "\"");
    if (modem.waitResponse() != 1) { Log::error(TAG, "MQTT Username failed"); return false; }

    modem.sendAT("+SMCONF=\"PASSWORD\",\"", password, "\"");
    if (modem.waitResponse() != 1) { Log::error(TAG, "MQTT Password failed"); return false; }
  } else {
    // The modem retains SMCONF entries across SMDISC, so a previous
    // user/pass cycle leaves USERNAME/PASSWORD set. Brokers using
    // use_identity_as_username (mTLS-only ACLs) reject the connect
    // when both cert AND user/pass arrive. Clear them explicitly.
    modem.sendAT("+SMCONF=\"USERNAME\",\"\"");
    modem.waitResponse();
    modem.sendAT("+SMCONF=\"PASSWORD\",\"\"");
    modem.waitResponse();
    Log::info(TAG, "mTLS active - using client cert (no user/pass)");
  }

  // TLS 1.2
  modem.sendAT("+CSSLCFG=\"SSLVERSION\",0,3");
  modem.waitResponse();

  if (wantMTLS) {
    // CSSLCFG CONVERT slot semantics on SIM7080G (fw 1951B17 verified):
    //   type 1 = client certificate, takes 4-arg form with separate
    //            key filename: CONVERT,1,"<cert>","<key>"
    //   type 2 = CA list (single PEM file)
    // Sequence + filenames mirror the LilyGo SIMCom samples (AWS IoT +
    // ATT mosquitto) - bench-validated wedge pattern; using SMSSL=2 +
    // a concatenated cert/key file aborted the TLS handshake mid-way
    // even though CONVERT 2 returned OK. The 4-arg CONVERT 1 flow with
    // separate cert/key files is what the modem actually wires through
    // to mbedtls for client auth.
    modem.sendAT("+CSSLCFG=\"CONVERT\",2,\"server-ca.crt\"");
    if (modem.waitResponse() != 1) { Log::error(TAG, "CA convert failed"); return false; }

    modem.sendAT("+CSSLCFG=\"CONVERT\",1,\"client.crt\",\"client.key\"");
    if (modem.waitResponse() != 1) { Log::error(TAG, "Client cert convert failed"); return false; }

    // Optional SNI: AWS IoT + Cloudflare-fronted brokers need it to
    // route to the right backend. Mosquitto direct (no SNI required)
    // ignores it harmlessly. CSSLCFG SNI is per-CTX (index 0 = default
    // SSL context, the one SMSSL drives).
    char sniCmd[160];
    snprintf(sniCmd, sizeof(sniCmd), "+CSSLCFG=\"sni\",0,\"%s\"", broker);
    modem.sendAT(sniCmd);
    modem.waitResponse();

    // SMSSL mode 1 with both CA + client cert filenames = mutual TLS.
    // Mode 1 is the catch-all "verify server + present client cert if
    // configured" knob on this fw rev; mode 2 has different semantics
    // and bench-failed the handshake. The client-cert side is wired
    // by the prior CSSLCFG CONVERT 1 call referencing client.crt + key.
    modem.sendAT("+SMSSL=1,\"server-ca.crt\",\"client.crt\"");
    if (modem.waitResponse() != 1) { Log::error(TAG, "mTLS SSL attach failed"); return false; }
  } else if (_hasCACert) {
    modem.sendAT("+CSSLCFG=\"CONVERT\",2,\"server-ca.crt\"");
    if (modem.waitResponse() != 1) { Log::error(TAG, "CA convert failed"); return false; }

    modem.sendAT("+SMSSL=1,\"server-ca.crt\",\"\"");
    if (modem.waitResponse() != 1) { Log::error(TAG, "SSL attach failed"); return false; }
  } else {
    modem.sendAT("+SMSSL=0,\"\",\"\"");
    modem.waitResponse();
    Log::warn(TAG, "MQTT TLS without CA verification");
  }

  Log::info(TAG, "Connecting MQTT...");
  modem.sendAT("+SMCONN");
  if (waitChunked(30000UL) != 1) {
    // SIM7080G emits the actual cause (CME/SSL error) hundreds of ms
    // to a few seconds after the bare ERROR; 5 s window catches the
    // full chatter so the log line carries the real reason.
    String tail = drainModemTail(5000UL);
    char line[384];
    snprintf(line, sizeof(line), "MQTT connect failed: %s", tail.c_str());
    Log::error(TAG, line);
    return false;
  }

  Log::info(TAG, "MQTT connected");

  // Re-issue every WiFi-side subscription on the cellular MQTT session
  //. Reconnect path benefits too - the modem drops subscriptions
  // on SMDISC so we have to repaint them on every fresh SMCONN.
  if (!smsubAll()) {
    Log::warn(TAG, "smsubAll failed - inbound MQTT may be partial");
  }
  return true;
}

// ---------------------------------------------------------------------------

// Parse a single +SMSUB: URC line and dispatch to MQTTClient. Shared
// by pumpInbound and cellAT so both code paths deliver URCs identically.
//
// in:  line   NUL-terminated +SMSUB: URC line (no trailing CRLF)
// out: none   - dispatched via MQTTClient::dispatchInbound on success
void Cellular::routeSmsubLine(char* line) {
  // Format: +SMSUB: "<topic>","<payload>"
  char* p = line + 8;
  if (*p != '"') return;
  p++;
  char* topicStart = p;
  char* topicEnd   = strchr(p, '"');
  if (!topicEnd) return;
  *topicEnd = '\0';
  p = topicEnd + 1;
  if (*p != ',' || *(p + 1) != '"') return;
  p += 2;
  char* payloadStart = p;
  char* payloadEnd   = strrchr(p, '"');
  if (!payloadEnd || payloadEnd == p - 1) return;
  *payloadEnd = '\0';

  char dmsg[160];
  snprintf(dmsg, sizeof(dmsg), "RX %s (%u B)", topicStart,
           (unsigned)(payloadEnd - payloadStart));
  Log::info(TAG, dmsg);

  MQTTClient::dispatchInbound(topicStart, payloadStart,
                              (size_t)(payloadEnd - payloadStart));
}

// URC-safe AT command. See header for contract.
int Cellular::cellAT(const char* cmd, const char* expect, uint32_t timeoutMs,
                     std::function<void(const char*)> lineCallback) {
  // Drain any bytes already in the modem stream so we do not mix
  // straggler URCs into our response window. Any +SMSUB: lines we
  // find here are dispatched immediately.
  {
    char    line[512];
    size_t  lineLen = 0;
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      if (c == '\r') continue;
      if (c == '\n') {
        line[lineLen] = '\0';
        if (strncmp(line, "+SMSUB: ", 8) == 0) routeSmsubLine(line);
        lineLen = 0;
        continue;
      }
      if (lineLen < sizeof(line) - 1) line[lineLen++] = c;
    }
  }

  // Send command.
  Serial1.print("AT");
  Serial1.print(cmd);
  Serial1.print("\r\n");

  uint32_t end = millis() + timeoutMs;
  uint32_t lastWdt = millis();
  char     line[512];
  size_t   lineLen = 0;

  while (millis() < end) {
    // Watchdog feed every 500 ms so callers can pass long timeouts
    // without tripping the 5 s task watchdog.
    if (millis() - lastWdt > 500) {
      esp_task_wdt_reset();
      lastWdt = millis();
    }
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      if (c == '\r') continue;
      if (c == '\n') {
        line[lineLen] = '\0';
        // Empty line: skip (the modem prefixes responses with a CRLF).
        if (lineLen == 0) continue;

        // URC -> dispatch and continue waiting for our response.
        if (strncmp(line, "+SMSUB: ", 8) == 0) {
          routeSmsubLine(line);
          lineLen = 0;
          continue;
        }

        // Terminator?
        if (strcmp(line, expect) == 0) return 1;
        if (strcmp(line, "ERROR") == 0) return -1;
        // CME / CMS errors carry detail; treat as ERROR for return code
        // but pass the line through so callers can log the cause.
        bool isErr = (strncmp(line, "+CME ERROR", 10) == 0 ||
                      strncmp(line, "+CMS ERROR", 10) == 0);
        if (lineCallback) lineCallback(line);
        if (isErr) return -1;

        lineLen = 0;
        continue;
      }
      if (lineLen < sizeof(line) - 1) {
        line[lineLen++] = c;
      } else {
        // Line overflow - reset to avoid corrupt parses.
        Log::warn(TAG, "cellAT line overflow - dropping");
        lineLen = 0;
      }
    }
    delay(2);
  }
  return 0;  // timeout
}

// URC-safe SMPUB-style AT command. See header for contract.
//
// Why this exists: TinyGSM's waitResponse (used by Cellular::publish's
// previous AT+SMPUB path) does not recognize +SMSUB: URCs and silently
// consumes them out of the modem stream. Bench evidence (diag fork,
// 1 s burst pubs against a wildcard sub): only 2/5 URCs delivered when
// the firmware self-publishes a cli/response between bursts. With
// 5 s spacing 5/5 deliver. cellATWrite reads bytes itself and routes
// any +SMSUB: line through routeSmsubLine the same way pumpInbound and
// cellAT do.
int Cellular::cellATWrite(const char* cmd, const uint8_t* payload,
                          size_t payloadLen, uint32_t promptTimeoutMs,
                          uint32_t okTimeoutMs) {
  // Phase 0: drain any straggler bytes already in the stream and route
  // URCs (same as cellAT). Anything left over from a prior call must
  // not pollute our prompt/response window.
  {
    char    line[512];
    size_t  lineLen = 0;
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      if (c == '\r') continue;
      if (c == '\n') {
        line[lineLen] = '\0';
        if (strncmp(line, "+SMSUB: ", 8) == 0) routeSmsubLine(line);
        lineLen = 0;
        continue;
      }
      if (lineLen < sizeof(line) - 1) line[lineLen++] = c;
    }
  }

  // Phase 1: send command, wait for '>' prompt. Assemble lines in
  // parallel so any +SMSUB: line arriving before the prompt is still
  // routed (the modem can interleave URCs and the prompt). The prompt
  // is a single '>' byte with no LF, so we scan byte-by-byte.
  Serial1.print("AT");
  Serial1.print(cmd);
  Serial1.print("\r\n");

  uint32_t end     = millis() + promptTimeoutMs;
  uint32_t lastWdt = millis();
  char     line[512];
  size_t   lineLen = 0;
  bool     gotPrompt = false;

  while (!gotPrompt && millis() < end) {
    if (millis() - lastWdt > 500) {
      esp_task_wdt_reset();
      lastWdt = millis();
    }
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      if (c == '>') { gotPrompt = true; break; }
      if (c == '\r') continue;
      if (c == '\n') {
        line[lineLen] = '\0';
        if (lineLen > 0) {
          if (strncmp(line, "+SMSUB: ", 8) == 0) routeSmsubLine(line);
          else if (strcmp(line, "ERROR") == 0)  return -1;
          else if (strncmp(line, "+CME ERROR", 10) == 0 ||
                   strncmp(line, "+CMS ERROR", 10) == 0) return -1;
        }
        lineLen = 0;
        continue;
      }
      if (lineLen < sizeof(line) - 1) {
        line[lineLen++] = c;
      } else {
        Log::warn(TAG, "cellATWrite line overflow - dropping");
        lineLen = 0;
      }
    }
    if (!gotPrompt) delay(2);
  }
  if (!gotPrompt) return 0;

  // Phase 2: write payload bytes.
  Serial1.write(payload, payloadLen);

  // Phase 3: wait for OK terminator. Same line loop as cellAT, with
  // URC routing.
  end     = millis() + okTimeoutMs;
  lastWdt = millis();
  lineLen = 0;

  while (millis() < end) {
    if (millis() - lastWdt > 500) {
      esp_task_wdt_reset();
      lastWdt = millis();
    }
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      if (c == '\r') continue;
      if (c == '\n') {
        line[lineLen] = '\0';
        if (lineLen == 0) continue;
        if (strncmp(line, "+SMSUB: ", 8) == 0) {
          routeSmsubLine(line);
          lineLen = 0;
          continue;
        }
        if (strcmp(line, "OK") == 0)    return 1;
        if (strcmp(line, "ERROR") == 0) return -1;
        if (strncmp(line, "+CME ERROR", 10) == 0 ||
            strncmp(line, "+CMS ERROR", 10) == 0) return -1;
        lineLen = 0;
        continue;
      }
      if (lineLen < sizeof(line) - 1) {
        line[lineLen++] = c;
      } else {
        Log::warn(TAG, "cellATWrite line overflow - dropping");
        lineLen = 0;
      }
    }
    delay(2);
  }
  return 0;  // timeout
}

// Check if the modem is registered on the cellular network. URC-safe:
// goes through cellAT instead of TinyGSM's getRegistrationStatus so a
// +SMSUB: URC arriving during the response window is not eaten by the
// modem.waitResponse(OK) parser.
bool Cellular::isRegistered() {
  // Try +CEREG? first (LTE-M / NB-IoT). Fall back to +CGREG? if the
  // modem reports only GPRS-style registration. Format:
  //   +CEREG: <n>,<stat>[,...]
  //   stat values: 1 = registered home, 5 = registered roaming
  bool home = false, roaming = false;
  cellAT("+CEREG?", "OK", 2000UL, [&](const char* line) {
    const char* p = strstr(line, "+CEREG:");
    if (!p) return;
    p = strchr(p, ',');
    if (!p) return;
    int stat = atoi(p + 1);
    if (stat == 1) home    = true;
    if (stat == 5) roaming = true;
  });
  if (home || roaming) return true;
  cellAT("+CGREG?", "OK", 2000UL, [&](const char* line) {
    const char* p = strstr(line, "+CGREG:");
    if (!p) return;
    p = strchr(p, ',');
    if (!p) return;
    int stat = atoi(p + 1);
    if (stat == 1) home    = true;
    if (stat == 5) roaming = true;
  });
  return (home || roaming);
}

// URC-safe replacement for modem.isGprsConnected(). Parses +CNACT? line
//   +CNACT: 0,<state>,<ip>
// state == 1 means the data context is active.
bool Cellular::isGprsConnectedRaw() {
  bool active = false;
  cellAT("+CNACT?", "OK", 2000UL, [&](const char* line) {
    const char* p = strstr(line, "+CNACT:");
    if (!p) return;
    p = strchr(p, ',');
    if (!p) return;
    int state = atoi(p + 1);
    if (state == 1) active = true;
  });
  return active;
}

// URC-safe MQTT session check. Parses +SMSTATE: <state>; state == 1
// means an MQTT session is up.
bool Cellular::mqttIsConnected() {
  bool up = false;
  cellAT("+SMSTATE?", "OK", 2000UL, [&](const char* line) {
    const char* p = strstr(line, "+SMSTATE:");
    if (!p) return;
    p += 9;
    while (*p == ' ') p++;
    if (atoi(p) == 1) up = true;
  });
  return up;
}

// ---------------------------------------------------------------------------

// Power up the modem only. PMU rails on, PWRKEY pulsed, AT verified.
// Idempotent on _modemAlive. Independent of network/MQTT bring-up so
// the GNSS module can warm the modem without taking over network duty.
bool Cellular::powerOn() {
  if (_modemAlive) return true;

  ATGuard g(120000UL);
  if (!g.ok()) {
    Log::error(TAG, "powerOn: AT bus stuck");
    return false;
  }

  initPMU();
  if (!wakeModem()) {
    Log::error(TAG, "Modem did not wake");
    return false;
  }
  _modemAlive = true;
  return true;
}

// --- Incremental activation state machine ----------------------------------
// begin() used to run power-on through MQTT-connect in one blocking call,
// freezing every other module loop for 30-120 s (Forgejo FW M1). The
// tickActivation() state machine below walks the same bring-up one phase
// per call instead; CellularModule polls it every loop while ACTIVATING.
// State is module-static - only one activation runs at a time and
// CellularModule is its sole driver.
enum class ActPhase : uint8_t {
  INIT,       // install hooks, stamp the deadline
  POWER_ON,   // PMU rails + PWRKEY + AT probe
  SIM,        // PIN unlock, SIM_READY check, CA cert upload
  RADIO_CFG,  // CFUN cycle, band/APN config, RF settle
  REGISTER,   // poll network registration, one read per tick
  BEARER,     // activate the PDP data context
  MQTT,       // modem-native MQTT connect (non-blocking backoff on fail)
};
static ActPhase s_actPhase     = ActPhase::INIT;
static uint32_t s_actDeadline  = 0;  // millis() hard stop for the cycle
static uint32_t s_regPollStart = 0;  // millis() the REGISTER phase began
static uint32_t s_lastRegPoll  = 0;  // millis() of the last registration read
static uint32_t s_nextRetryMs  = 0;  // millis() before which a failed MQTT
                                     // connect will not be retried

// Advance the cellular bring-up by one phase. See Cellular.h for the
// caller contract. Each phase takes its own short-lived ATGuard so the
// AT bus is free between phases and other module loops keep ticking.
Cellular::ActStatus Cellular::tickActivation() {
  // Already fully up (re-entry after a WiFi-recovery yield): nothing to do.
  if (_started) { s_actPhase = ActPhase::INIT; return ActStatus::DONE; }

  uint32_t now = millis();

  // Overall deadline guard. A wedged modem or bad broker config must not
  // pin the device in ACTIVATING forever - after the timeout we hardware-
  // reset the modem and report FAILED so CellularModule drops to STANDBY
  // (device stays responsive, keeps watching WiFi, retries later).
  if (s_actPhase != ActPhase::INIT && s_actDeadline != 0 &&
      (int32_t)(now - s_actDeadline) >= 0) {
    Log::error(TAG, "Cellular activation timed out - hard-resetting modem");
    hardReset();
    s_actPhase = ActPhase::INIT;
    return ActStatus::FAILED;
  }

  switch (s_actPhase) {
    case ActPhase::INIT: {
      Log::info(TAG, "Starting cellular path...");
      // Wire the MQTTClient cert-cleared hook so cert.clear from any path
      // (Shell, MQTT CLI, WS) drops the modem-side cache + active session
      // and triggers a reconnect under the new auth mode. Idempotent.
      MQTTClient::setOnClientCertCleared([]() {
        Cellular::invalidateClientCert();
      });
      JsonObject cfg     = Config::get();
      uint32_t   timeout = cfg["cellular"]["activation_timeout_ms"] | 1800000UL;
      s_actDeadline = now + timeout;
      s_nextRetryMs = 0;
      mqttBackoffReset();  // fresh cycle starts at the short backoff window
      s_actPhase    = ActPhase::POWER_ON;
      return ActStatus::PENDING;
    }

    case ActPhase::POWER_ON: {
      // powerOn() takes its own (long) ATGuard internally; idempotent if
      // the modem is already alive (e.g. GNSS warmed it earlier).
      if (!powerOn()) {
        Log::error(TAG, "Modem powerOn failed - aborting cellular bring-up");
        s_actPhase = ActPhase::INIT;
        return ActStatus::FAILED;
      }
      s_actPhase = ActPhase::SIM;
      return ActStatus::PENDING;
    }

    case ActPhase::SIM: {
      ATGuard g(120000UL);
      if (!g.ok()) {
        Log::error(TAG, "Cellular SIM phase: AT bus stuck - aborting");
        s_actPhase = ActPhase::INIT;
        return ActStatus::FAILED;
      }
      JsonObject  cfg = Config::get();
      const char* pin = cfg["cellular"]["sim_pin"] | "";
      if (strlen(pin) > 0 && modem.getSimStatus() == SIM_LOCKED) {
        if (!modem.simUnlock(pin)) {
          Log::error(TAG, "SIM unlock failed");
          s_actPhase = ActPhase::INIT;
          return ActStatus::FAILED;
        }
        Log::info(TAG, "SIM unlocked");
      }
      if (modem.getSimStatus() != SIM_READY) {
        Log::error(TAG, "SIM not ready - cellular unavailable");
        s_actPhase = ActPhase::INIT;
        return ActStatus::FAILED;
      }
      Log::info(TAG, "SIM ready");
      writeCACert();
      s_actPhase = ActPhase::RADIO_CFG;
      return ActStatus::PENDING;
    }

    case ActPhase::RADIO_CFG: {
      ATGuard g(120000UL);
      if (!g.ok()) {
        Log::warn(TAG, "Cellular radio config: AT bus busy - retrying");
        return ActStatus::PENDING;
      }
      radioConfigure();
      s_regPollStart = millis();
      s_lastRegPoll  = 0;
      s_actPhase     = ActPhase::REGISTER;
      return ActStatus::PENDING;
    }

    case ActPhase::REGISTER: {
      // Poll registration at ~1 Hz so the bring-up does not hammer the
      // AT bus every main-loop iteration.
      if (s_lastRegPoll != 0 && (now - s_lastRegPoll) < 1000UL) {
        return ActStatus::PENDING;
      }
      s_lastRegPoll = now;
      ATGuard g(10000UL);
      if (!g.ok()) return ActStatus::PENDING;  // bus busy - retry next tick

      int r = pollRegistration();
      if (r > 0) {
        s_actPhase = ActPhase::BEARER;
        return ActStatus::PENDING;
      }
      // Not registered. On denial or the per-attempt registration
      // timeout, re-walk the radio config (the cycle deadline still
      // bounds the total).
      JsonObject cfg        = Config::get();
      uint32_t   regTimeout = cfg["cellular"]["reg_timeout_ms"] | 180000;
      if (r < 0 || (millis() - s_regPollStart) > regTimeout) {
        Log::warn(TAG, "Registration failed - re-walking radio config");
        s_actPhase = ActPhase::RADIO_CFG;
      }
      return ActStatus::PENDING;
    }

    case ActPhase::BEARER: {
      ATGuard g(60000UL);
      if (!g.ok()) return ActStatus::PENDING;
      if (!activateBearer()) {
        Log::warn(TAG, "Bearer activation failed - re-walking radio config");
        s_actPhase = ActPhase::RADIO_CFG;
        return ActStatus::PENDING;
      }
      s_actPhase = ActPhase::MQTT;
      return ActStatus::PENDING;
    }

    case ActPhase::MQTT: {
      // Non-blocking backoff: after a failed connect, just wait out
      // s_nextRetryMs without holding the bus so other modules tick.
      if (s_nextRetryMs != 0 && (int32_t)(now - s_nextRetryMs) < 0) {
        return ActStatus::PENDING;
      }
      ATGuard g(120000UL);
      if (!g.ok()) return ActStatus::PENDING;

      if (mqttConnect()) {
        mqttBackoffReset();
        _started          = true;
        _mqttConnected    = true;
        _publishGate      = true;
        _lastSignalSample = 0;
        sampleSignalQuality();
        s_actPhase = ActPhase::INIT;
        return ActStatus::DONE;
      }

      // Failed - schedule the next attempt and grow the backoff window.
      s_nextRetryMs = millis() + s_mqttRetryMs;
      uint32_t next = s_mqttRetryMs * 2UL;
      s_mqttRetryMs = (next > MQTT_RETRY_MAX_MS) ? MQTT_RETRY_MAX_MS : next;
      char msg[64];
      snprintf(msg, sizeof(msg), "MQTT connect failed - retry in %lu s",
               (unsigned long)((s_nextRetryMs - millis()) / 1000UL));
      Log::warn(TAG, msg);
      return ActStatus::PENDING;
    }
  }
  return ActStatus::PENDING;  // unreachable - keeps the compiler happy
}

// ---------------------------------------------------------------------------

// Cellular-only recovery: re-register on network loss, reconnect MQTT on drop.
// WiFi-vs-cellular policy is handled by CellularModule, not here.
//
// Steady-state ordering:
//   1. pumpInbound() drains +SMSUB: URCs. Self-guarded with a 0-tick
//      try-acquire so it steps aside instantly when any AT caller (this
//      task or another) holds the bus.
//   2. Once per HEALTH_PROBE_MS, take an ATGuard, run the TinyGSM checks
//      (isRegistered / isGprsConnected / mqttIsConnected / sampleSignalQuality),
//      release on scope exit.
//   3. If a probe finds the link broken, run the existing recovery
//      (networkConnect + mqttConnect) inside the same guard scope so URCs
//      do not interleave with AT chatter.
void Cellular::loop() {
  if (!_started) return;

  pumpInbound();

  uint32_t now = millis();
  if (s_lastHealthMs != 0 && (now - s_lastHealthMs) < HEALTH_PROBE_MS) {
    return;  // pumpInbound owns Serial1 the rest of the time
  }
  s_lastHealthMs = now;

  ATGuard g;
  if (!g.ok()) {
    // Another task holds the bus past the 30 s budget - skip this tick
    // and try next loop. Health probe is best-effort; a stuck guard will
    // recover when the holder releases.
    Log::warn(TAG, "Health probe skipped - AT bus busy");
    return;
  }
  esp_task_wdt_reset();

  // Recovery: network dropped.
  if (!isRegistered() || !isGprsConnectedRaw()) {
    esp_task_wdt_reset();
    Log::warn(TAG, "Network lost - re-registering");
    _mqttConnected = false;
    while (!networkConnect()) {
      Log::warn(TAG, "Retry network in 10s");
      // Release the AT bus + pump shell during the sleep so restart /
      // cell.* / config.* commands stay reachable while we hammer
      // re-registration. Without this the main loop is frozen until
      // the modem recovers, and a stuck modem can require a physical
      // power cycle to escape.
      g.pause(10000UL);
    }
    while (!mqttConnect()) {
      mqttBackoffWait(&g);
    }
    mqttBackoffReset();
    _mqttConnected = true;
    return;
  }

  esp_task_wdt_reset();

  // Recovery: MQTT dropped, network still up.
  if (!mqttIsConnected()) {
    esp_task_wdt_reset();
    Log::warn(TAG, "MQTT dropped - reconnecting");
    _mqttConnected = false;
    if (mqttConnect()) {
      _mqttConnected = true;
      mqttBackoffReset();
    } else {
      mqttBackoffWait(&g);
    }
  } else if (!_mqttConnected) {
    // Modem session is up but our flag drifted false (e.g. a slow SMPUB
    // OK timed out at 5 s and we marked _mqttConnected=false; the OK
    // eventually arrived and the broker thinks we're still subscribed).
    // Re-sync the flag so Cellular::publish stops silently dropping.
    Log::info(TAG, "MQTT flag re-synced: modem session is up");
    _mqttConnected = true;
  }

  esp_task_wdt_reset();
  sampleSignalQuality();
  esp_task_wdt_reset();
}

// ---------------------------------------------------------------------------

// True when modem is up AND the publish gate is open. Subscribers gate on
// this so a brief WiFi recovery does not require tearing the modem down.
bool Cellular::connected() {
  return _started && _mqttConnected && _publishGate;
}

// ---------------------------------------------------------------------------

// Toggle the publish gate. Closing it stops cellular publishes immediately
// while leaving the MQTT session warm for fast re-takeover. CellularModule
// uses this on yield (WiFi recovered) and resume (WiFi dropped again).
void Cellular::setPublishGate(bool open) {
  _publishGate = open;
}

// ---------------------------------------------------------------------------

// Last sampled signal quality from AT+CSQ. Range 0..31 (higher is better),
// 99 means unknown / no sample yet.
int Cellular::getSignalQuality() {
  return _signalQuality;
}

// ---------------------------------------------------------------------------

// True once the modem has been woken and is AT-responsive. GNSS callers
// gate on this; cellular registration / MQTT need not be up.
bool Cellular::isModemAlive() {
  return _modemAlive;
}

// ---------------------------------------------------------------------------

// Atomic GNSS fix-acquisition cycle for SIM7080G. Per long-running
// community testing (undocumented in vendor PDFs): the modem time-shares
// its radio between LTE and GNSS. While CGNSPWR=1 the LTE data path is
// suspended; TCP/TLS connections (incl. modem-native +SMCONN MQTT) stay
// registered but cannot push bytes. Issuing CFUN=0 to switch tears the
// TCP session down - the right pattern is just CGNSPWR=1, then on the
// way back CGNSPWR=0 followed by CFUN=1 to wake the LTE data path. The
// broker delivers anything sent during the GNSS window the moment
// CFUN=1 returns.
//
// ATGuard is held across the entire window so pumpInbound, publish()
// and the health probe cannot race the AT bus. The window is bounded
// by timeoutMs and the watchdog is fed every second so a 60 s cold fix
// does not trip TWDT. Guard timeout = timeoutMs + 10 s slack so the
// outer caller never wins the race against itself on a long cold fix.
bool Cellular::gpsAcquireFix(uint32_t timeoutMs,
                             float* lat, float* lon,
                             float* alt, float* speed,
                             int* satsInView, int* satsUsed) {
  if (!_modemAlive) return false;

  ATGuard g(timeoutMs + 10000UL);
  if (!g.ok()) {
    Log::warn(TAG, "GPS fix skipped - AT bus busy");
    return false;
  }

  bool fix = false;
  if (modem.enableGPS()) {
    uint32_t end = millis() + timeoutMs;
    float fLat = 0, fLon = 0, fAlt = 0, fSpeed = 0, fAcc = 0;
    int   vSat = 0, uSat = 0;
    while (millis() < end) {
      if (modem.getGPS(&fLat, &fLon, &fSpeed, &fAlt, &vSat, &uSat, &fAcc)) {
        fix = true;
        if (lat)        *lat        = fLat;
        if (lon)        *lon        = fLon;
        if (alt)        *alt        = fAlt;
        if (speed)      *speed      = fSpeed;
        if (satsInView) *satsInView = vSat;
        if (satsUsed)   *satsUsed   = uSat;
        break;
      }
      // 1 s poll cadence; faster polling burns AT bus without helping
      // the receiver. wdt fed every poll so cold-fix windows up to
      // 60 s stay TWDT-safe.
      delay(1000);
      esp_task_wdt_reset();
    }
    modem.disableGPS();
  }

  // Wake the LTE data path. Without this, even with CGNSPWR=0, the modem
  // keeps LTE data suspended and the next SMPUB silently fails. CFUN=1
  // returns OK fast but the data path needs a short settle before the
  // modem-native MQTT session is publish-ready - poll SMSTATE? until it
  // reports 1 (connected) or until a short timeout, so the next
  // Cellular::publish does not catch the half-asleep state.
  modem.sendAT("+CFUN=1");
  modem.waitResponse(5000UL);

  uint32_t settleEnd = millis() + 5000UL;
  while (millis() < settleEnd) {
    modem.sendAT("+SMSTATE?");
    int state = -1;
    if (modem.waitResponse(2000UL, "+SMSTATE: ") == 1) {
      state = modem.stream.parseInt();
      modem.waitResponse();
    }
    if (state == 1) break;
    delay(250);
    esp_task_wdt_reset();
  }

  return fix;
}

// ---------------------------------------------------------------------------

// Send a raw AT command directly to Serial1 (the SIM7080 UART) and stream
// the response back via `emit`. Bypasses TinyGSM so it works regardless of
// the Cellular state machine - useful for live debugging operator scans,
// band locks, registration cause codes etc. without reflashing.
//
// Pass `cmd` without the "AT" prefix. Reads until "\r\nOK\r\n" / "\r\nERROR\r\n"
// or `timeoutMs` elapses. Slow commands (operator scan +COPS=?) need
// >= 120 s.
void Cellular::atPassthrough(const char* cmd, uint32_t timeoutMs,
                             std::function<void(const char*)> emit) {
  ATGuard g(timeoutMs + 5000UL);
  if (!g.ok()) {
    emit("ERROR: AT bus busy");
    return;
  }
  Serial1.printf("AT%s\r\n", cmd);
  uint32_t t0 = millis();
  String buf;
  buf.reserve(512);
  while (millis() - t0 < timeoutMs) {
    while (Serial1.available()) buf += (char)Serial1.read();
    if (buf.indexOf("\r\nOK\r\n") != -1)    break;
    if (buf.indexOf("\r\nERROR\r\n") != -1) break;
    delay(20);
    esp_task_wdt_reset();
  }
  int start = 0;
  for (int i = 0; i < (int)buf.length(); ++i) {
    if (buf[i] == '\n') {
      String line = buf.substring(start, i);
      line.trim();
      if (line.length()) emit(line.c_str());
      start = i + 1;
    }
  }
  if (start < (int)buf.length()) {
    String line = buf.substring(start);
    line.trim();
    if (line.length()) emit(line.c_str());
  }
}

// ---------------------------------------------------------------------------

// Refresh _signalQuality from AT+CSQ at most every SIGNAL_SAMPLE_MS to avoid
// monopolising the AT bus. Called from begin() and loop() while started.
// Goes through cellAT so any +SMSUB: URC arriving during the response
// window is delivered, not eaten by a TinyGSM-style waitResponse parser.
//
void Cellular::sampleSignalQuality() {
  uint32_t now = millis();
  if (_lastSignalSample != 0 && (now - _lastSignalSample) < SIGNAL_SAMPLE_MS) return;
  _lastSignalSample = now;

  int rssi = 99;  // unknown
  cellAT("+CSQ", "OK", 2000UL, [&](const char* line) {
    // Format: +CSQ: <rssi>,<ber>
    const char* p = strstr(line, "+CSQ:");
    if (!p) return;
    p += 5;
    while (*p == ' ') p++;
    rssi = atoi(p);  // 0..31 valid, 99 = unknown
  });
  _signalQuality = rssi;
}

// ---------------------------------------------------------------------------

// Publish a message via modem-native MQTT (AT+SMPUB).
// retain=true sets the AT+SMPUB retain flag so messages whose semantics
// require broker-side retention (HA discovery, retained-topics manifest,
// LWT availability) carry the same flag over the cellular leg.
//
// Enforces a 1 Hz minimum spacing between successive publishes
// (defensive guard against the burst-wedge pattern: ~10 SMPUBs landing
// in the first ~10 s of a fresh cellular MQTT session can wedge URC
// routing on the SIM7080G even with cellATWrite routing URCs through
// the prompt/OK windows). Bounded delay (<= 1000 ms) leaves TWDT
// margin; ATGuard is acquired after the delay so other callers do
// not stall behind a sleeping one.
bool Cellular::publish(const char* topic, const char* payload, bool retain) {
  if (!connected()) return false;

  // Rate-limit before AT-bus acquire so concurrent callers wait for the
  // mutex, not for a sleeping publisher. Each successful publish updates
  // _lastSmpubMs; the next caller pays the remaining quantum.
  uint32_t now = millis();
  if (_lastSmpubMs != 0 && (now - _lastSmpubMs) < SMPUB_MIN_SPACING_MS) {
    uint32_t wait = SMPUB_MIN_SPACING_MS - (now - _lastSmpubMs);
    while (wait > 0) {
      uint32_t step = wait > 200 ? 200 : wait;
      delay(step);
      esp_task_wdt_reset();
      wait -= step;
    }
  }

  ATGuard g;
  if (!g.ok()) {
    Log::warn(TAG, "Publish skipped - AT bus busy");
    return false;
  }
  _lastSmpubMs = millis();

  size_t len = strlen(payload);
  char cmd[128];
  snprintf(cmd, sizeof(cmd), "+SMPUB=\"%s\",%d,1,%d",
           topic, (int)len, retain ? 1 : 0);

  // Use cellATWrite (raw Serial1 + URC routing) instead of TinyGSM's
  // sendAT/waitResponse pair. waitResponse does not recognize +SMSUB:
  // URCs, so any URC arriving inside the SMPUB '>' prompt or OK
  // terminator window is silently consumed. cellATWrite routes those
  // lines through routeSmsubLine -> dispatchInbound the same way
  // pumpInbound and cellAT do.
  int rc = cellATWrite(cmd, (const uint8_t*)payload, len, 5000UL, 8000UL);
  if (rc == 1) return true;

  Log::warn(TAG, rc == 0 ? "SMPUB timeout" : "SMPUB failed");
  _mqttConnected = false;
  return false;
}

// ---------------------------------------------------------------------------

// Subscribe to a single MQTT topic on the cellular MQTT session via
// AT+SMSUB. QoS 1 to match the WiFi-side default. Returns true on OK
// response.
//
// No connected() guard here on purpose: smsubAll is called from inside
// mqttConnect right after SMCONN succeeds, but _started/_mqttConnected
// are not flipped until mqttConnect returns. The AT command itself
// fails fast if the session is not actually up.
bool Cellular::smsub(const char* topic) {
  ATGuard g;
  if (!g.ok()) {
    Log::warn(TAG, "SMSUB skipped - AT bus busy");
    return false;
  }
  esp_task_wdt_reset();
  modem.sendAT("+SMSUB=\"", topic, "\",1");
  int res = modem.waitResponse(3000UL);
  esp_task_wdt_reset();
  if (res != 1) {
    char wmsg[80];
    snprintf(wmsg, sizeof(wmsg), "SMSUB failed for %s", topic);
    Log::warn(TAG, wmsg);
    return false;
  }
  return true;
}

// Issue AT+SMSUB for every active subscription registered with MQTTClient.
//
// Bench observation on this SIM7080G modem firmware revision: with the
// DC3 power-cycle fix in place,
//   - Literal topics                          : reliable
//   - Single-level `+` (e.g. cli/+)           : reliable
//   - Multi-level  `#` (e.g. cli/#, <prefix>/#): drops URCs under burst
// So we mirror exactly what MQTTClient::_subs holds (cli/+ and literal
// cmd/* topics) and never widen to `#`.
//
// MAX_SUBS=4 stays under the SIM7080 session cap.
bool Cellular::smsubAll() {
  bool allOk = true;
  static constexpr int MAX_SUBS = 4;
  int  count = 0;
  MQTTClient::forEachSubscription([&](const char* topic) {
    if (count >= MAX_SUBS) {
      Log::warn(TAG, "SMSUB cap reached - extra subs not registered on cellular");
      return;
    }
    if (!smsub(topic)) allOk = false;
    count++;
  });
  char msg[80];
  snprintf(msg, sizeof(msg), "SMSUB %d topic(s) on cellular MQTT", count);
  Log::info(TAG, msg);
  return allOk;
}

// Drain Serial1 for any pending +SMSUB: URCs and dispatch matching
// payloads via MQTTClient::dispatchInbound (same callback path WiFi
// uses). Non-blocking: reads at most until the next \n or until the
// stream is empty.
//
// SIM7080 +SMSUB URC format:
//   +SMSUB: "<topic>","<payload>"\r\n
//
// Topics and payloads are double-quoted ASCII. Embedded quotes / binary
// in payloads is out of scope for v1 (filed).
//
// Stream ownership: this takes the AT bus mutex with a 0-tick try-acquire;
// if any caller (this task or another) currently holds the bus we step
// aside and wait for the next loop iteration. The mutex makes the check
// race-free where the previous cooperative s_atBusy flag was not.
void Cellular::pumpInbound() {
  if (!_started) return;
  ATGuard g(0);
  if (!g.ok()) return;

  static char    line[512];
  static size_t  lineLen = 0;

  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[lineLen] = '\0';
#ifdef THESADA_CELL_DEBUG
      // Diag: log every line so we can see exactly what reaches pumpInbound.
      if (lineLen > 0) {
        char dmsg[160];
        snprintf(dmsg, sizeof(dmsg), "rx: %s", line);
        Log::info(TAG, dmsg);
      }
#endif
      // Look for the +SMSUB: prefix; ignore everything else (other URCs,
      // stray AT responses that escaped TinyGSM's parser).
      if (strncmp(line, "+SMSUB: ", 8) == 0) routeSmsubLine(line);
      lineLen = 0;
      continue;
    }
    if (lineLen < sizeof(line) - 1) {
      line[lineLen++] = c;
    } else {
      // Line overflow - reset, drop the rest. Logged once.
      Log::warn(TAG, "pumpInbound line overflow - dropping");
      lineLen = 0;
    }
  }
}

// ---------------------------------------------------------------------------

// HTTPS GET via TinyGsmClientSecure. See header for contract.
//
// Coexistence with modem-native MQTT: TinyGsmClientSecure on SIM7080G
// drives AT+CAOPEN / AT+CASEND / AT+CARECV which use the modem's
// general-purpose TCP socket slots, while AT+SMCONN holds the dedicated
// MQTT session on its own internal slot. They do not collide in
// practice. We still take the AT-bus mutex for the whole call so other
// AT traffic on this firmware (CSQ probes, cell.at calls, the MQTT
// pumpInbound URC drain) serialises behind the request.
//
// Status-line + header parsing is line-oriented: collect bytes until
// CRLF, dispatch on the line, switch to body-streaming after the empty
// line separator. Each body chunk reaches `writeCallback` directly so
// the caller can stream to Update.write() / mbedtls_sha256_update()
// without ever buffering the full payload.
bool Cellular::httpsGet(const char* host, const char* path, uint16_t port,
                        std::function<bool(const uint8_t*, size_t)> writeCallback,
                        int* httpStatus,
                        size_t rangeStart, size_t rangeEndInclusive) {
  if (httpStatus) *httpStatus = 0;
  if (!_modemAlive) {
    Log::warn(TAG, "httpsGet: modem not alive");
    return false;
  }

  // Generous overall budget: TLS handshake against a CDN can take 8 s,
  // and a multi-MB firmware download is the eventual caller (typical
  // 1.5 MB binary @ ~8 KB/s LTE-M = ~3 min). Cap at 15 min so a slow or
  // stalled cellular link gives up gracefully instead of holding the AT
  // bus forever.
  ATGuard g(15UL * 60UL * 1000UL);
  if (!g.ok()) {
    Log::warn(TAG, "httpsGet: AT bus busy");
    return false;
  }

  // TinyGsmClientSecure holds its rx_buffer (TINY_GSM_RX_BUFFER, 4 KB on
  // cellular envs) as a member array. Stack-allocating it would consume
  // half of the 8 KB Arduino loop task stack and risk overflow under a
  // deep call chain. Heap allocation routes through PSRAM on ESP32-S3
  // when internal SRAM is tight (CONFIG_SPIRAM_USE_MALLOC + the
  // arduino-esp32 default policy), leaving the loop stack free.
  auto* clientPtr = new (std::nothrow) TinyGsmClientSecure(modem, 0);
  if (!clientPtr) {
    Log::error(TAG, "httpsGet: heap allocation failed");
    return false;
  }
  std::unique_ptr<TinyGsmClientSecure> clientOwner(clientPtr);
  TinyGsmClientSecure& client = *clientPtr;
  client.setTimeout(30000);

  {
    char msg[200];
    snprintf(msg, sizeof(msg), "httpsGet: connect %s:%u", host, port);
    Log::info(TAG, msg);
  }
  if (!client.connect(host, port, 30)) {
    Log::error(TAG, "httpsGet: connect failed");
    return false;
  }

  // Build + send GET. Keep request small (well under MTU) so a single
  // CASEND covers it. Optional Range header for chunked downloads.
  char rangeHdr[64];
  rangeHdr[0] = '\0';
  if (rangeEndInclusive > 0) {
    snprintf(rangeHdr, sizeof(rangeHdr),
             "Range: bytes=%u-%u\r\n",
             (unsigned)rangeStart, (unsigned)rangeEndInclusive);
  }
  char req[500];
  int reqLen = snprintf(req, sizeof(req),
    "GET %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Connection: close\r\n"
    "User-Agent: thesada-fw\r\n"
    "Accept: */*\r\n"
    "%s"
    "\r\n",
    path, host, rangeHdr);
  if (reqLen <= 0 || reqLen >= (int)sizeof(req)) {
    Log::error(TAG, "httpsGet: request buffer overflow");
    client.stop();
    return false;
  }
  client.write((const uint8_t*)req, reqLen);

  // Read response: status line, headers, then stream body.
  enum { STATUS, HEADERS, BODY } phase = STATUS;
  char lineBuf[256];
  size_t lineLen = 0;
  int status = 0;
  int contentLen = -1;  // -1 = unknown / chunked / no header
  size_t bodyBytes = 0;
  // Per-stall budget: how long the body loop will wait with zero bytes
  // available before giving up. Cellular reads can pause briefly at chunk
  // boundaries (CARECV cycle); a TinyGSM "disconnected" reading needs to
  // be cross-checked against this rather than trusted blindly, because
  // TinyGSM on SIM7080G flips its internal connected flag as soon as the
  // server-side FIN arrives - even when ~1 MB is still buffered modem-side
  // waiting for CARECV to drain. Empirically a multi-second idle is the
  // signal we've actually run out of data, not the connected flag.
  static constexpr uint32_t kBodyIdleTimeoutMs = 15000UL;
  uint32_t deadline = millis() + 5UL * 60UL * 1000UL;  // 5 min global cap
  uint32_t lastWdt  = millis();
  uint32_t lastByte = millis();

  while (millis() < deadline) {
    // Belt + braces: feed task watchdog every 200 ms even if the body
    // loop never sleeps. Yield each iteration so the Arduino loop's
    // other cooperative tasks (USB CDC, Shell pump, etc) get cycles
    // while we're holding the AT bus for a multi-minute download.
    if (millis() - lastWdt > 200) {
      esp_task_wdt_reset();
      lastWdt = millis();
    }
    yield();

    if (phase != BODY) {
      // Header phase: read line-by-line. Bail if connection drops before
      // headers complete (server hangup mid-response).
      if (!client.connected() && !client.available()) {
        Log::warn(TAG, "httpsGet: disconnected before headers");
        break;
      }
      if (!client.available()) { delay(10); continue; }
      while (client.available()) {
        char c = (char)client.read();
        if (c == '\r') continue;
        if (c == '\n') {
          lineBuf[lineLen] = '\0';
          if (phase == STATUS) {
            // "HTTP/1.1 200 OK"
            const char* sp = strchr(lineBuf, ' ');
            if (sp) status = atoi(sp + 1);
            if (httpStatus) *httpStatus = status;
            phase = HEADERS;
          } else if (phase == HEADERS) {
            if (lineLen == 0) {
              // Empty line: header/body separator.
              phase = BODY;
              lineLen = 0;
              lastByte = millis();
              break;
            }
            if (strncasecmp(lineBuf, "content-length:", 15) == 0) {
              contentLen = atoi(lineBuf + 15);
            }
          }
          lineLen = 0;
        } else {
          if (lineLen < sizeof(lineBuf) - 1) lineBuf[lineLen++] = c;
        }
      }
      continue;
    }

    // Body phase. Trust Content-Length over TinyGSM's connected flag:
    // exit when we have the full payload, or after kBodyIdleTimeoutMs of
    // no data when the server hasn't told us how big the body is.
    if (contentLen > 0 && bodyBytes >= (size_t)contentLen) break;

    // 1024 B per read keeps each CARECV cycle aligned with a full TCP
    // segment from the modem's socket buffer, reducing the number of
    // round-trips needed to drain a multi-MB download. Larger reads risk
    // TinyGSM ring buffer overflow on TINY_GSM_RX_BUFFER (set to 4096
    // in platformio.ini for the cellular envs).
    uint8_t chunk[1024];
    int n = client.read(chunk, sizeof(chunk));
    if (n > 0) {
      bodyBytes += n;
      lastByte = millis();
      if (writeCallback && !writeCallback(chunk, n)) {
        Log::warn(TAG, "httpsGet: callback aborted transfer");
        client.stop();
        if (httpStatus) *httpStatus = status;
        return false;
      }
      continue;
    }
    // No bytes this iteration. Decide between wait and exit.
    if (millis() - lastByte > kBodyIdleTimeoutMs) {
      if (contentLen > 0 && bodyBytes < (size_t)contentLen) {
        char msg[120];
        snprintf(msg, sizeof(msg),
                 "httpsGet: body stall %u/%d bytes after %u ms idle - aborting",
                 (unsigned)bodyBytes, contentLen,
                 (unsigned)(millis() - lastByte));
        Log::error(TAG, msg);
      }
      break;
    }
    delay(20);
  }

  client.stop();

  if (status == 0) {
    Log::error(TAG, "httpsGet: no status line received");
    return false;
  }
  {
    char msg[120];
    snprintf(msg, sizeof(msg),
             "httpsGet: done status=%d body=%u bytes (content-length header=%d)",
             status, (unsigned)bodyBytes, contentLen);
    Log::info(TAG, msg);
  }
  // Treat short body (got fewer bytes than Content-Length declared) as a
  // transport failure so the OTA caller does not run SHA on a truncated
  // stream and report a hash mismatch. Status code stays correct.
  if (contentLen > 0 && bodyBytes < (size_t)contentLen) return false;
  return true;
}

// ---------------------------------------------------------------------------
// net.* transport-abstraction helpers (wired into Net::CellularProvider)
// ---------------------------------------------------------------------------

// True when the modem is registered and the CNACT data context is up.
// Both sub-checks go through cellAT, which needs the AT-bus mutex held -
// so this takes a short ATGuard for the whole probe.
//
// In:  none (reads modem state over the AT bus)
// Out: bool - true when DNS / NTP / HTTPS over the modem can be expected
//      to work.
bool Cellular::dataLinkUp() {
  if (!_modemAlive) return false;
  ATGuard g(5000UL);
  if (!g.ok()) return false;
  return isRegistered() && isGprsConnectedRaw();
}

// ---------------------------------------------------------------------------

// Close the data context and modem MQTT session, leaving the modem
// powered and registered. Clears _started so a future re-takeover
// re-walks begin() - powerOn() is then a no-op (modem still alive),
// writeCACert() is gated by _hasCACert, networkConnect() re-opens CNACT
// and mqttConnect() re-establishes the session.
//
// In:  none (AT bus via internal ATGuard)
// Out: CNACT slot 0 closed; modem stays CFUN=1 registered.
void Cellular::dataLinkDown() {
  if (!_modemAlive) return;
  ATGuard g(15000UL);
  if (!g.ok()) {
    Log::warn(TAG, "dataLinkDown: AT bus stuck - skipping teardown");
    return;
  }
  modem.sendAT("+SMDISC");
  modem.waitResponse(5000UL);
  modem.sendAT("+CNACT=0,0");
  modem.waitResponse(5000UL);
  _started       = false;
  _mqttConnected = false;
  _publishGate   = false;
  Log::info(TAG, "Cellular data context closed (modem still registered)");
}

// Emit a human-readable summary of the cellular link: operator, modem IP,
// signal quality, IMEI. Best-effort - any field that cannot be read is
// reported as unknown rather than aborting the whole dump.
//
// In:  emit - sink called once per output line
// Out: 3-4 lines emitted via the callback
void Cellular::netInfo(std::function<void(const char*)> emit) {
  ATGuard g(10000UL);
  if (!g.ok()) { emit("cellular: AT bus busy"); return; }
  char line[128];

  // Operator: AT+COPS? -> +COPS: <mode>,<format>,"<oper>"[,<act>]
  String oper;
  cellAT("+COPS?", "OK", 5000UL, [&](const char* l) {
    const char* p = strstr(l, "+COPS:");
    if (!p) return;
    const char* q = strchr(p, '"');
    if (!q) return;
    q++;
    const char* e = strchr(q, '"');
    if (e && e > q) oper = String(q).substring(0, e - q);
  });
  snprintf(line, sizeof(line), "operator: %s",
           oper.length() ? oper.c_str() : "(unknown)");
  emit(line);

  // Modem IP: AT+CNACT? -> +CNACT: <pdpidx>,<status>,<addr>
  String ip;
  cellAT("+CNACT?", "OK", 3000UL, [&](const char* l) {
    const char* p = strstr(l, "+CNACT:");
    if (!p) return;
    const char* c1 = strchr(p, ',');
    if (!c1) return;
    const char* c2 = strchr(c1 + 1, ',');
    if (!c2) return;
    // Status field sits between c1 and c2. Only the active context (1)
    // carries a real address; inactive PDP indices report 0.0.0.0 and
    // would clobber the IP since this callback fires per +CNACT: line.
    const char* st = c1 + 1;
    while (*st == ' ') st++;
    if (*st != '1') return;
    const char* a = c2 + 1;
    while (*a == ' ' || *a == '"') a++;
    String s(a);
    int q = s.indexOf('"');
    if (q >= 0) s = s.substring(0, q);
    s.trim();
    ip = s;
  });
  snprintf(line, sizeof(line), "ip: %s", ip.length() ? ip.c_str() : "(none)");
  emit(line);

  // Signal: cached AT+CSQ value (0..31 valid, 99 = unknown). Convert the
  // valid range to dBm for an operator-friendly number.
  int csq = _signalQuality;
  if (csq >= 0 && csq <= 31) {
    snprintf(line, sizeof(line), "signal: %d/31 (%d dBm)", csq, -113 + 2 * csq);
  } else {
    snprintf(line, sizeof(line), "signal: unknown");
  }
  emit(line);

  // IMEI: AT+GSN emits the bare 15-digit IMEI on its own line before OK.
  String imei;
  cellAT("+GSN", "OK", 3000UL, [&](const char* l) {
    const char* p = l;
    while (*p == ' ') p++;
    size_t n = strlen(p);
    bool digits = n >= 14 && n <= 17;
    for (size_t i = 0; i < n && digits; ++i) {
      if (p[i] < '0' || p[i] > '9') digits = false;
    }
    if (digits) imei = p;
  });
  if (imei.length()) {
    snprintf(line, sizeof(line), "imei: %s", imei.c_str());
    emit(line);
  }
}

// Resolve a hostname via the modem DNS (AT+CDNSGIP). The modem answers
// OK immediately, then sends the result as a +CDNSGIP: URC a moment
// later, so we issue the command via cellAT and then drain Serial1 for
// the URC line.
//   +CDNSGIP: 1,"host","IP1"[,"IP2"]   on success
//   +CDNSGIP: 0,<err>                  on failure
//
// In:  host    hostname to resolve
//      out     buffer for the dotted-quad result (NUL-terminated)
//      outLen  size of out
// Out: bool - true and out filled on success
bool Cellular::resolveHost(const char* host, char* out, size_t outLen) {
  if (!host || !out || outLen == 0) return false;
  out[0] = '\0';

  ATGuard g(15000UL);
  if (!g.ok()) return false;

  // recount 2, per-try timeout 5000 ms.
  char cmd[160];
  snprintf(cmd, sizeof(cmd), "+CDNSGIP=\"%s\",2,5000", host);
  if (cellAT(cmd, "OK", 8000UL, nullptr) != 1) return false;

  // Drain Serial1 for the +CDNSGIP: URC (arrives after the OK).
  char     foundIp[48] = {0};
  bool     resolved    = false;
  uint32_t t0          = millis();
  String   buf;
  buf.reserve(256);
  while (millis() - t0 < 10000UL) {
    while (Serial1.available()) buf += (char)Serial1.read();
    int idx = buf.indexOf("+CDNSGIP:");
    if (idx >= 0) {
      int nl = buf.indexOf('\n', idx);
      if (nl < 0) { delay(20); esp_task_wdt_reset(); continue; }
      String l = buf.substring(idx, nl);
      // +CDNSGIP: 1,"host","ip"...  -> status field is 1
      int firstComma = l.indexOf(',');
      if (firstComma > 0 && l.substring(9, firstComma).indexOf('1') >= 0) {
        int q1 = l.indexOf('"', firstComma);          // open host
        int q2 = l.indexOf('"', q1 + 1);              // close host
        int q3 = l.indexOf('"', q2 + 1);              // open ip
        int q4 = l.indexOf('"', q3 + 1);              // close ip
        if (q3 > 0 && q4 > q3) {
          String ipStr = l.substring(q3 + 1, q4);
          strncpy(foundIp, ipStr.c_str(), sizeof(foundIp) - 1);
          resolved = true;
        }
      }
      break;
    }
    delay(20);
    esp_task_wdt_reset();
  }

  if (resolved) {
    strncpy(out, foundIp, outLen - 1);
    out[outLen - 1] = '\0';
  }
  return resolved;
}

// Sync the ESP32 system clock off the modem. Runs AT+CNTP against
// `server`, waits for the +CNTP: <code> URC, then reads the modem RTC
// with AT+CCLK? and pushes the UTC epoch to settimeofday().
//
// Why both steps: AT+CNTP sets the *modem* RTC, not the ESP32 clock; the
// CCLK read + settimeofday is what actually moves the system clock.
//
// In:  server     NTP server hostname
//      timeoutMs  upper bound on the +CNTP: result wait
// Out: bool - true once settimeofday() ran with a post-2023 epoch
bool Cellular::ntpSync(const char* server, uint32_t timeoutMs) {
  if (!server) return false;

  ATGuard g(timeoutMs + 5000UL);
  if (!g.ok()) return false;

  // CNTPCID must be 0 on the SIM7080 (TinyGsmClientSIM7080 notes CID 1
  // does not work on this part).
  cellAT("+CNTPCID=0", "OK", 5000UL, nullptr);

  // tz 0 -> modem keeps UTC; the ESP32 clock stores a UTC epoch.
  char cmd[128];
  snprintf(cmd, sizeof(cmd), "+CNTP=\"%s\",0", server);
  if (cellAT(cmd, "OK", 5000UL, nullptr) != 1) return false;

  // Execute the sync. Result is a +CNTP: <code> URC after the OK; the
  // SIM7080 reports code 1 on success.
  if (cellAT("+CNTP", "OK", 5000UL, nullptr) != 1) return false;

  int      code = 0;
  uint32_t t0   = millis();
  String   buf;
  buf.reserve(128);
  while (millis() - t0 < timeoutMs) {
    while (Serial1.available()) buf += (char)Serial1.read();
    int idx = buf.indexOf("+CNTP:");
    if (idx >= 0) {
      int nl = buf.indexOf('\n', idx);
      if (nl < 0) { delay(20); esp_task_wdt_reset(); continue; }
      code = atoi(buf.c_str() + idx + 6);
      break;
    }
    delay(50);
    esp_task_wdt_reset();
  }
  if (code != 1) return false;

  // Modem RTC is now UTC. Read it and push to the ESP32 clock.
  //   +CCLK: "yy/MM/dd,hh:mm:ss+zz"
  struct tm tmv      = {};
  bool      gotClock = false;
  cellAT("+CCLK?", "OK", 3000UL, [&](const char* l) {
    const char* p = strstr(l, "+CCLK:");
    if (!p) return;
    p = strchr(p, '"');
    if (!p) return;
    p++;
    int yy, mo, dd, hh, mi, ss;
    if (sscanf(p, "%d/%d/%d,%d:%d:%d", &yy, &mo, &dd, &hh, &mi, &ss) == 6) {
      tmv.tm_year = yy + 100;  // modem gives 2-digit year >= 2000
      tmv.tm_mon  = mo - 1;
      tmv.tm_mday = dd;
      tmv.tm_hour  = hh;
      tmv.tm_min   = mi;
      tmv.tm_sec   = ss;
      tmv.tm_isdst = 0;
      gotClock = true;
    }
  });
  if (!gotClock) return false;

  // mktime() interprets tmv in the process TZ. This path only runs when
  // WiFi never associated, so WiFiManager's configTime() never set a
  // non-UTC zone - the default newlib TZ is UTC, making mktime == UTC
  // here. Matches the mktime use in Shell.cpp's `net.ntp set`.
  time_t epoch = mktime(&tmv);
  if (epoch < 1700000000L) return false;
  struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  return true;
}

#endif // ENABLE_CELLULAR
