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
int               Cellular::_signalQuality  = 99;
uint32_t          Cellular::_lastSignalSample = 0;
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

// Serial port for TinyGSM
static TinyGsm modem(Serial1);

// Sample AT+CSQ at most every 30 s to keep AT bus clean.
static constexpr uint32_t SIGNAL_SAMPLE_MS = 30UL * 1000UL;

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
// in:  none (uses module-static s_mqttRetryMs)
// out: none
static void mqttBackoffWait() {
  uint32_t ms = s_mqttRetryMs;
  char msg[64];
  snprintf(msg, sizeof(msg), "Retry MQTT in %lu s", (unsigned long)(ms / 1000UL));
  Log::warn(TAG, msg);
  uint32_t remaining = ms;
  while (remaining) {
    uint32_t step = remaining > 500UL ? 500UL : remaining;
    delay(step);
    esp_task_wdt_reset();
    remaining -= step;
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

// Configure radio, register on LTE-M network, and activate data bearer
bool Cellular::networkConnect() {
  JsonObject cfg = Config::get();
  const char* apn         = cfg["cellular"]["apn"]            | "OSC";
  uint32_t    rfSettleMs  = cfg["cellular"]["rf_settle_ms"]   | 15000;
  uint32_t    regTimeout  = cfg["cellular"]["reg_timeout_ms"] | 180000;

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
  // Chunk the delay so the task watchdog stays fed; whole sequence runs
  // inside a single loop() tick that otherwise blocks > TWDT timeout.
  char msg[64];
  snprintf(msg, sizeof(msg), "RF settle %lu ms...", rfSettleMs);
  Log::info(TAG, msg);
  {
    uint32_t remaining = rfSettleMs;
    while (remaining) {
      uint32_t step = remaining > 500UL ? 500UL : remaining;
      delay(step);
      esp_task_wdt_reset();
      remaining -= step;
    }
  }

  Log::info(TAG, "Waiting for registration...");
  SIM70xxRegStatus s;
  uint32_t start = millis();
#ifdef THESADA_CELL_DEBUG
  uint32_t lastDiag = 0;
#endif
  do {
    esp_task_wdt_reset();
    Shell::pumpConsole();
    s = modem.getRegistrationStatus();
    if (s == REG_OK_HOME)    { Log::info(TAG, "Registered - HOME");    break; }
    if (s == REG_OK_ROAMING) { Log::info(TAG, "Registered - ROAMING"); break; }
    if (s == REG_DENIED)     { Log::error(TAG, "Registration DENIED"); return false; }
#ifdef THESADA_CELL_DEBUG
    if (millis() - lastDiag > 5000UL) {
      lastDiag = millis();
      char dbg[160];
      int csq = modem.getSignalQuality();
      snprintf(dbg, sizeof(dbg), "diag: regStatus=%d CSQ=%d", (int)s, csq);
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
    delay(1000);
    if (millis() - start > regTimeout) {
      Log::error(TAG, "Registration timeout");
      return false;
    }
  } while (true);

  // Activate bearer.
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

  modem.sendAT("+SMCONF=\"KEEPTIME\",60");
  modem.waitResponse();

  modem.sendAT("+SMCONF=\"CLEANSS\",1");
  modem.waitResponse();

  modem.sendAT("+SMCONF=\"CLIENTID\",\"", devName, "\"");
  if (modem.waitResponse() != 1) { Log::error(TAG, "MQTT ClientID failed"); return false; }

  modem.sendAT("+SMCONF=\"USERNAME\",\"", user, "\"");
  if (modem.waitResponse() != 1) { Log::error(TAG, "MQTT Username failed"); return false; }

  modem.sendAT("+SMCONF=\"PASSWORD\",\"", password, "\"");
  if (modem.waitResponse() != 1) { Log::error(TAG, "MQTT Password failed"); return false; }

  // TLS 1.2
  modem.sendAT("+CSSLCFG=\"SSLVERSION\",0,3");
  modem.waitResponse();

  if (_hasCACert) {
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
    String tail = drainModemTail();
    char line[256];
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

// Initialize PMU, modem, SIM, CA cert, network, and MQTT connection.
// Calls powerOn() first; if the modem is already alive (e.g. GNSS
// warmed it earlier), powerOn is a no-op. Returns immediately if the
// full path has already completed.
void Cellular::begin() {
  if (_started) return;  // already fully initialised
  Log::info(TAG, "Starting cellular path...");

  if (!powerOn()) {
    Log::error(TAG, "Modem powerOn failed - aborting cellular bring-up");
    return;
  }

  // Take the AT bus for the rest of the bring-up. Long: registration
  // alone can take 60-180 s on first SIM-card touch, plus CA upload +
  // SMCONN. Any cross-task caller that hits Cellular::publish during
  // boot will wait its turn rather than racing the registration AT
  // chatter.
  ATGuard g(300000UL);
  if (!g.ok()) {
    Log::error(TAG, "Cellular begin: AT bus stuck - aborting");
    return;
  }

  // Unlock SIM with PIN if configured.
  {
    JsonObject  cfg = Config::get();
    const char* pin = cfg["cellular"]["sim_pin"] | "";
    if (strlen(pin) > 0) {
      if (modem.getSimStatus() == SIM_LOCKED) {
        if (!modem.simUnlock(pin)) {
          Log::error(TAG, "SIM unlock failed");
          return;
        }
        Log::info(TAG, "SIM unlocked");
      }
    }
  }

  if (modem.getSimStatus() != SIM_READY) {
    Log::error(TAG, "SIM not ready - cellular unavailable");
    return;
  }
  Log::info(TAG, "SIM ready");

  writeCACert();

  // Retry network + MQTT until both succeed.
  while (!networkConnect()) {
    Log::warn(TAG, "Network connect failed, retry in 10s");
    for (int i = 0; i < 20; ++i) { delay(500); esp_task_wdt_reset(); }
  }
  while (!mqttConnect()) {
    mqttBackoffWait();
  }
  mqttBackoffReset();

  _started          = true;
  _mqttConnected    = true;
  _publishGate      = true;
  _lastSignalSample = 0;
  sampleSignalQuality();
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
      for (int i = 0; i < 20; ++i) { delay(500); esp_task_wdt_reset(); }
    }
    while (!mqttConnect()) {
      mqttBackoffWait();
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
      mqttBackoffWait();
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

// Publish a message via modem-native MQTT (AT+SMPUB)
bool Cellular::publish(const char* topic, const char* payload) {
  if (!connected()) return false;

  ATGuard g;
  if (!g.ok()) {
    Log::warn(TAG, "Publish skipped - AT bus busy");
    return false;
  }

  size_t len = strlen(payload);
  char cmd[128];
  snprintf(cmd, sizeof(cmd), "+SMPUB=\"%s\",%d,1,0", topic, (int)len);

  // waitChunked feeds the watchdog every 1.5 s so we can give the modem
  // a generous timeout for slow SMPUB cycles without tripping the 5 s
  // task watchdog.
  modem.sendAT(cmd);
  if (waitChunked(5000UL, ">") != 1) {
    Log::warn(TAG, "No SMPUB prompt");
    _mqttConnected = false;
    return false;
  }

  modem.stream.write(payload, len);

  if (waitChunked(8000UL) == 1) return true;

  Log::warn(TAG, "SMPUB failed");
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

#endif // ENABLE_CELLULAR
