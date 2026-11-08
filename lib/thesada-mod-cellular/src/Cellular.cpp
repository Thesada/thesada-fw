// thesada-fw - Cellular.cpp
// SIM7080G Cat-M1: power, registration, data bearer and plain TCP sockets.
// TLS and MQTT run on the ESP32 (TlsClient, MQTTClient) over those sockets.
// SPDX-License-Identifier: GPL-3.0-only

#include <thesada_config.h>
#ifdef ENABLE_CELLULAR

#include "Cellular.h"
#include <Identity.h>
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
#include <TlsClient.h>
#include <WiFiManager.h>
#include <clock_floor_policy.h>
#include <http_fetch_policy.h>
#include <boot_wait_policy.h>
#include <cell_recover_policy.h>
#include <http_url_policy.h>
#include <atomic>

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

// IDF logs every feed from a task the watchdog does not watch.
static void feedIfWatched() {
  if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
}

// Take the bus in fed slices of 1 s at most: the loop and net.http tasks
// both wait on it, and either can hold it past the 30 s watchdog. 0 = one try.
static bool atTake(SemaphoreHandle_t m, uint32_t timeoutMs) {
  if (timeoutMs == 0) return xSemaphoreTakeRecursive(m, 0) == pdTRUE;
  const uint32_t t0 = millis();
  for (uint32_t step; (step = waitSliceMs(t0, millis(), timeoutMs)) != 0; feedIfWatched()) {
    if (xSemaphoreTakeRecursive(m, pdMS_TO_TICKS(step)) == pdTRUE) return true;
  }
  return false;
}

Cellular::ATGuard::ATGuard(uint32_t timeoutMs) : _held(false) {
  atMutexInit();
  if (_atMutex == nullptr) return;  // OOM at create time - degrade open
  _held = atTake(_atMutex, timeoutMs);
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
    _held = atTake(_atMutex, 60000UL);
    if (!_held) Log::warn("Cellular", "cellular.at_reacquire_timeout");
  }
}

class ThesadaModem : public TinyGsm {
 public:
  explicit ThesadaModem(Stream& s) : TinyGsm(s) {}
  bool openSocket(const char* host, uint16_t port, uint8_t mux, int timeoutS) {
    return modemConnect(host, port, mux, false, timeoutS);
  }
};
static ThesadaModem modem(Serial1);

// Open-failure bookkeeping, defined after waitChunked below.
static void socketOpenFailed();
static void socketOpenOk();
static void socketSendFailed();
static void socketSendOk();

// One plain modem TCP socket. Lives for the whole program, so TinyGSM's
// sockets[] table never points at a freed client, and with slot 0 always
// registered its maintain() availability refresh (which only starts from
// sockets[0]) covers every slot. connect/stop are bounded so a slow
// network cannot outlast the 30 s task watchdog.
class ModemSocket : public TinyGsm::GsmClientSim7080 {
 public:
  ModemSocket(ThesadaModem& m, uint8_t mux) : GsmClientSim7080(m, mux) {}
  // The modem can still hold this cid after a timed-out open or a close
  // it reported only by URC, and then refuses every later CAOPEN on it,
  // so the close is sent whatever our flag says, before and after.
  bool open(const char* host, uint16_t port, int timeoutS) {
    close(true);
    sock_connected = static_cast<ThesadaModem*>(at)->openSocket(host, port, mux, timeoutS);
    if (!sock_connected) close(true);
    return sock_connected;
  }
  void close(bool always = false) {
    if (sock_connected || always) {
      at->sendAT(GF("+CACLOSE="), mux);
      at->waitResponse(3000);
    }
    sock_connected = false;
    sock_available = 0;
    got_data       = false;
    rx.clear();
  }
  bool   isOpen() const   { return sock_connected; }
  size_t buffered()       { return rx.size() + sock_available; }
  void   forget()         { sock_connected = false; sock_available = 0; rx.clear(); }
  // Ask the next maintain() for a fresh modem-side byte count now instead
  // of after TinyGSM's 500 ms idle throttle.
  void   pollNow()        { got_data = true; prev_check = millis(); }
};

// Per-socket counters for the httpsGet done line: where a slow transfer
// spends its time.
struct SocketStats { uint32_t reads, readBytes, readMs, polls, availCalls; };
static SocketStats s_stats[Cellular::TCP_SLOTS] = {};

// Set while one httpsGet owns socket slot 1.
static std::atomic<uint32_t> s_httpsBusy{0};

// Arduino Client over one ModemSocket. Every modem exchange takes the AT
// bus guard for its own duration only, so a long transfer never locks
// out the health probe or the other socket.
class ModemClient : public Client {
 public:
  ModemClient(ThesadaModem& m, uint8_t mux) : _s(m, mux), _mux(mux) {}

  // Resolves with the bounded +CDNSGIP first: a slow lookup inside
  // CAOPEN holds the bus past the other socket's keepalive.
  int connect(const char* host, uint16_t port) override {
    char ip[48];
    IPAddress parsed;
    if (parsed.fromString(host)) {
      strncpy(ip, host, sizeof(ip) - 1);
      ip[sizeof(ip) - 1] = '\0';
    } else if (!Cellular::resolveHost(host, ip, sizeof(ip))) {
      Log::kvfw(TAG, "cellular.socket_dns_failed host=%s", host);
      return 0;
    }
    Cellular::ATGuard g(25000UL);
    if (!g.ok()) return 0;
    esp_task_wdt_reset();
    bool ok = _s.open(ip, port, 15);
    esp_task_wdt_reset();
    if (ok) socketOpenOk();
    else    socketOpenFailed();
    return ok ? 1 : 0;
  }
  int connect(IPAddress ip, uint16_t port) override {
    char host[16];
    snprintf(host, sizeof(host), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return connect(host, port);
  }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* buf, size_t size) override {
    Cellular::ATGuard g(10000UL);
    if (!g.ok() || !_s.isOpen()) return 0;
    size_t done = 0;
    while (done < size) {
      size_t chunk = size - done > 1024 ? 1024 : size - done;
      size_t n = _s.write(buf + done, chunk);
      if (n == 0) break;
      done += n;
      esp_task_wdt_reset();
    }
    if (done == size) socketSendOk();
    else              socketSendFailed();
    return done;
  }
  // While bytes are flowing, poll the modem every 30 ms: TinyGSM's own
  // 500 ms idle throttle caps a download at a few KB/s. Idle sockets keep
  // the slow cadence so a quiet MQTT session does not flood the AT bus.
  int available() override {
    Cellular::ATGuard g(0);
    if (!g.ok()) return (int)_s.buffered();
    uint32_t now = millis();
    s_stats[_mux].availCalls++;
    if (_s.buffered() == 0 && now - _lastData < 2000UL && now - _lastPoll >= 30UL) {
      _s.pollNow();
      _lastPoll = now;
      s_stats[_mux].polls++;
    }
    int n = _s.available();
    if (n > 0) _lastData = now;
    return n;
  }
  int read() override {
    uint8_t c;
    return read(&c, 1) == 1 ? c : -1;
  }
  int read(uint8_t* buf, size_t size) override {
    Cellular::ATGuard g(5000UL);
    if (!g.ok()) return -1;
    uint32_t t0 = millis();
    int n = _s.read(buf, size);
    s_stats[_mux].reads++;
    s_stats[_mux].readMs += millis() - t0;
    if (n > 0) s_stats[_mux].readBytes += n;
    return n > 0 ? n : -1;
  }
  int peek() override {
    Cellular::ATGuard g(5000UL);
    if (!g.ok()) return -1;
    return _s.peek();
  }
  void flush() override {}
  void stop() override {
    Cellular::ATGuard g(10000UL);
    if (g.ok()) _s.close();
    else        _s.forget();
  }
  uint8_t connected() override { return (_s.isOpen() || _s.buffered() > 0) ? 1 : 0; }
  operator bool() override { return connected(); }
  void forget() { _s.forget(); }

 private:
  ModemSocket _s;
  uint8_t     _mux;
  uint32_t    _lastData = 0;
  uint32_t    _lastPoll = 0;
};

// Constructed in place: a copy would leave sockets[mux] on the temporary.
static ModemClient  s_sock0(modem, 0);
static ModemClient  s_sock1(modem, 1);
static ModemClient* const s_sockets[Cellular::TCP_SLOTS] = { &s_sock0, &s_sock1 };

// Sample AT+CSQ at most every 30 s to keep AT bus clean.
static constexpr uint32_t SIGNAL_SAMPLE_MS = 30UL * 1000UL;

// Registration + bearer health probe cadence while the link is up.
static constexpr uint32_t HEALTH_PROBE_MS = 60UL * 1000UL;
static uint32_t           s_lastHealthMs   = 0;
// Registration + bearer verdict of the last probe; false during recovery.
static bool               s_linkUp         = false;

// TinyGSM waitResponse blocks the task for the full timeout without
// feeding the task watchdog. This wrapper splits any long wait into
// 1.5 s chunks with esp_task_wdt_reset() between, so SIM7080 commands
// that legitimately need 10-30 s (CNACT, CFUN, registration) do
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

// Consecutive failed socket opens, and consecutive failed sends. Either
// streak at 3 recycles the bearer, at 6 power-cycles the modem.
static uint8_t s_openFails = 0;
static uint8_t s_sendFails = 0;

// The task that runs activation and the probe, and a step queued off it.
static TaskHandle_t         s_driverTask = nullptr;
static std::atomic<uint8_t> s_recoverQueued{CELL_RECOVER_NONE};

// One recovery step. Caller holds the AT bus.
static void socketRecoverRun(uint8_t step) {
  if (step == CELL_RECOVER_RECYCLE) {
    Log::kvfw(TAG, "cellular.socket_recover action=bearer_recycle");
    for (ModemClient* sk : s_sockets) sk->forget();
    modem.sendAT("+CNACT=0,0");
    waitChunked(10000UL);
    modem.sendAT("+CNACT=0,1");
    waitChunked(30000UL);
  } else if (step == CELL_RECOVER_RESET) {
    Log::kvfe(TAG, "cellular.socket_recover action=hard_reset");
    Cellular::hardReset();
  }
}

static void socketRecover(uint8_t& streak, const char* kind) {
  ++streak;
  Log::kvfw(TAG, "cellular.socket_%s_failed streak=%u", kind, (unsigned)streak);
  // A refused open or send often takes the data context down with it on
  // this modem: probe on the next loop instead of up to a minute later.
  s_lastHealthMs = 0;
  const uint8_t step = cellRecoverStep(streak);
  if (step == CELL_RECOVER_RESET) streak = 0;
  if (step == CELL_RECOVER_NONE) return;
  if (cellRecoverHere(xTaskGetCurrentTaskHandle(), s_driverTask)) {
    socketRecoverRun(step);
    return;
  }
  // A worker task (net.http) leaves the long steps to the loop's next probe.
  s_recoverQueued.fetch_or(step);
  Log::kvfw(TAG, "cellular.socket_recover_queued action=%s",
            step == CELL_RECOVER_RESET ? "hard_reset" : "bearer_recycle");
}

static void socketOpenOk()     { s_openFails = 0; }
static void socketOpenFailed() { socketRecover(s_openFails, "open"); }
static void socketSendOk()     { s_sendFails = 0; }
static void socketSendFailed() { socketRecover(s_sendFails, "send"); }

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
  Log::warn(TAG, "cellular.modem_reset method=dc3_power_cycle");
  if (!PowerManager::resetModem()) {
    Log::error(TAG, "cellular.modem_reset_failed reason=pmu_reset_failed");
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
    Shell::pumpConsole();
    if (modem.testAT(1000UL)) {
      Log::info(TAG, "cellular.modem_reset_ok");
      modem.sendAT("+CMEE=2");
      modem.waitResponse(2000UL);
      return true;
    }
  }
  Log::error(TAG, "cellular.modem_reset_timeout");
  return false;
}

// Public wrapper for the hardware reset. Drops state flags so the next
// CellularModule::loop tick re-walks activation. Caller can be Shell,
// watchdog, or anything that needs to recover a wedged modem.
bool Cellular::hardReset() {
  ATGuard g(120000UL);
  if (!g.ok()) {
    Log::error(TAG, "cellular.hard_reset_failed reason=at_bus_stuck");
    return false;
  }
  bool ok = modemSoftReset();
  _modemAlive = ok;
  _started    = false;
  s_recoverQueued.store(CELL_RECOVER_NONE);
  // The power cycle closed every socket without a +CASTATE URC.
  for (ModemClient* s : s_sockets) s->forget();
  return ok;
}

// ---------------------------------------------------------------------------

// Configure AXP2101 PMU power rails for the modem via PowerManager's
// persistent static _pmu instance. A previous version of this function
// instantiated a stack-local XPowersPMU; on return its destructor called
// XPowersAXP2101::deinit() -> __wire->end() which nulled Wire1's TX
// buffer, crashing the next battery poll with "NULL TX buffer pointer".
void Cellular::initPMU() {
  if (!PowerManager::setModemRails()) {
    Log::error(TAG, "cellular.modem_rails_failed reason=pmu_unavailable");
    return;
  }
  Log::info(TAG, "cellular.pmu_ready");
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
      Log::error(TAG, "cellular.modem_wake_timeout");
      return false;
    }
    if (++retry > 6) {
      Log::info(TAG, "cellular.pwrkey_pulse");
      digitalWrite(PIN_MODEM_PWR, LOW);  delay(100); esp_task_wdt_reset();
      digitalWrite(PIN_MODEM_PWR, HIGH); delay(1000); esp_task_wdt_reset();
      digitalWrite(PIN_MODEM_PWR, LOW);
      retry = 0;
    }
  }
  Log::info(TAG, "cellular.modem_alive");

  // Verbose CME ERROR (text) so failure tail logs are actionable. Mode 2
  // returns the textual cause string after every CME ERROR.
  modem.sendAT("+CMEE=2");
  modem.waitResponse(2000UL);
  return true;
}

// ---------------------------------------------------------------------------

// Radio configuration block of the network bring-up: CFUN power-cycle,
// network/preferred-mode select, APN (CGDCONT/CNCFG), CEREG/CGREG URC
// enable, then a configurable RF-settle delay before registration is
// polled. Caller owns the AT bus. Called from the tickActivation()
// RADIO_CFG phase.
//
// The CFUN waits + RF settle are chunked so the task watchdog stays fed;
// the whole sequence is one ~40 s burst that otherwise blocks > TWDT.
void Cellular::radioConfigure() {
  JsonObject  cfg        = Config::get();
  const char* apn        = cfg["cellular"]["apn"]          | "OSC";
  uint32_t    rfSettleMs = cfg["cellular"]["rf_settle_ms"] | 15000;

  Log::info(TAG, "cellular.radio_configure");
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
  Log::kvf(TAG, "cellular.rf_settle ms=%lu", (unsigned long)rfSettleMs);
  uint32_t remaining = rfSettleMs;
  while (remaining) {
    uint32_t step = remaining > 500UL ? 500UL : remaining;
    delay(step);
    esp_task_wdt_reset();
    Shell::pumpConsole();
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
  if (s == REG_OK_HOME)    { Log::info(TAG, "cellular.registered network=home");    return 1; }
  if (s == REG_OK_ROAMING) { Log::info(TAG, "cellular.registered network=roaming"); return 1; }
  if (s == REG_DENIED)     { Log::error(TAG, "cellular.registration_denied");       return -1; }
#ifdef THESADA_CELL_DEBUG
  static uint32_t lastDiag = 0;
  if (millis() - lastDiag > 5000UL) {
    lastDiag = millis();
    Log::kvf(TAG, "cellular.reg_diag status=%d csq=%d",
             (int)s, modem.getSignalQuality());
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
      // Raw AT-protocol trace - kept free-text on purpose (kv would obscure it).
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

  Log::info(TAG, "cellular.bearer_activate");
  if (!modem.isGprsConnected()) {
    modem.sendAT("+CNACT=0,1");
    if (waitChunked(30000UL) != 1) {
      modem.gprsConnect(apn, "", "");
    }
  }

  if (!modem.isGprsConnected()) {
    Log::error(TAG, "cellular.bearer_failed");
    return false;
  }

  Log::kvf(TAG, "cellular.bearer_up ip=%s", modem.localIP().toString().c_str());
  return true;
}

// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------

// URC-safe AT command. See header for contract.
int Cellular::cellAT(const char* cmd, const char* expect, uint32_t timeoutMs,
                     std::function<void(const char*)> lineCallback) {
  // Hand straggler URCs (+CADATAIND, +CASTATE) to TinyGSM so socket
  // state stays right, then the response window starts clean.
  while (Serial1.available()) modem.waitResponse(15, nullptr, nullptr);

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
        Log::warn(TAG, "cellular.at_line_overflow src=cellat action=drop");
        lineLen = 0;
      }
    }
    delay(2);
  }
  return 0;  // timeout
}

// Check if the modem is registered on the cellular network.
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

// ---------------------------------------------------------------------------

// Power up the modem only. PMU rails on, PWRKEY pulsed, AT verified.
// Idempotent on _modemAlive. Independent of network/MQTT bring-up so
// the GNSS module can warm the modem without taking over network duty.
bool Cellular::powerOn() {
  if (_modemAlive) return true;

  ATGuard g(120000UL);
  if (!g.ok()) {
    Log::error(TAG, "cellular.power_on_failed reason=at_bus_stuck");
    return false;
  }

  initPMU();
  if (!wakeModem()) {
    Log::error(TAG, "cellular.power_on_failed reason=wake_timeout");
    return false;
  }
  _modemAlive = true;
  return true;
}

// --- Incremental activation state machine ----------------------------------
// begin() used to run power-on through MQTT-connect in one blocking call,
// freezing every other module loop for 30-120 s. The
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
};
static ActPhase s_actPhase     = ActPhase::INIT;
static uint32_t s_actDeadline  = 0;  // millis() hard stop for the cycle
static uint32_t s_regPollStart = 0;  // millis() the REGISTER phase began
static uint32_t s_lastRegPoll  = 0;  // millis() of the last registration read

// Advance the cellular bring-up by one phase. See Cellular.h for the
// caller contract. Each phase takes its own short-lived ATGuard so the
// AT bus is free between phases and other module loops keep ticking.
Cellular::ActStatus Cellular::tickActivation() {
  s_driverTask = xTaskGetCurrentTaskHandle();
  // Already fully up (re-entry after a WiFi-recovery yield): nothing to do.
  if (_started) { s_actPhase = ActPhase::INIT; return ActStatus::DONE; }

  uint32_t now = millis();

  // Overall deadline guard. A wedged modem or bad broker config must not
  // pin the device in ACTIVATING forever - after the timeout we hardware-
  // reset the modem and report FAILED so CellularModule drops to STANDBY
  // (device stays responsive, keeps watching WiFi, retries later).
  if (s_actPhase != ActPhase::INIT && s_actDeadline != 0 &&
      (int32_t)(now - s_actDeadline) >= 0) {
    Log::error(TAG, "cellular.activation_timeout action=hard_reset");
    hardReset();
    s_actPhase = ActPhase::INIT;
    return ActStatus::FAILED;
  }

  switch (s_actPhase) {
    case ActPhase::INIT: {
      Log::info(TAG, "cellular.activation_start");
      JsonObject cfg     = Config::get();
      uint32_t   timeout = cfg["cellular"]["activation_timeout_ms"] | 1800000UL;
      s_actDeadline = now + timeout;
      s_actPhase    = ActPhase::POWER_ON;
      return ActStatus::PENDING;
    }

    case ActPhase::POWER_ON: {
      // powerOn() takes its own (long) ATGuard internally; idempotent if
      // the modem is already alive (e.g. GNSS warmed it earlier).
      if (!powerOn()) {
        Log::error(TAG, "cellular.activation_failed phase=power_on");
        s_actPhase = ActPhase::INIT;
        return ActStatus::FAILED;
      }
      s_actPhase = ActPhase::SIM;
      return ActStatus::PENDING;
    }

    case ActPhase::SIM: {
      ATGuard g(120000UL);
      if (!g.ok()) {
        Log::error(TAG, "cellular.activation_failed phase=sim reason=at_bus_stuck");
        s_actPhase = ActPhase::INIT;
        return ActStatus::FAILED;
      }
      JsonObject  cfg = Config::get();
      const char* pin = cfg["cellular"]["sim_pin"] | "";
      if (strlen(pin) > 0 && modem.getSimStatus() == SIM_LOCKED) {
        if (!modem.simUnlock(pin)) {
          Log::error(TAG, "cellular.sim_unlock_failed");
          s_actPhase = ActPhase::INIT;
          return ActStatus::FAILED;
        }
        Log::info(TAG, "cellular.sim_unlocked");
      }
      if (modem.getSimStatus() != SIM_READY) {
        // CFUN=0 (minimum functionality) takes the SIM down with the radio.
        Log::warn(TAG, "cellular.sim_wait action=cfun1");
        modem.sendAT("+CFUN=1");
        modem.waitResponse(10000UL);
        for (int i = 0; i < 20 && modem.getSimStatus() != SIM_READY; i++) {
          delay(500);
          esp_task_wdt_reset();
          Shell::pumpConsole();
        }
      }
      if (modem.getSimStatus() != SIM_READY) {
        Log::error(TAG, "cellular.sim_not_ready");
        s_actPhase = ActPhase::INIT;
        return ActStatus::FAILED;
      }
      Log::info(TAG, "cellular.sim_ready");
      s_actPhase = ActPhase::RADIO_CFG;
      return ActStatus::PENDING;
    }

    case ActPhase::RADIO_CFG: {
      ATGuard g(120000UL);
      if (!g.ok()) {
        Log::warn(TAG, "cellular.radio_cfg_deferred reason=at_bus_busy");
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
        Log::warn(TAG, "cellular.registration_failed action=radio_reconfig");
        s_actPhase = ActPhase::RADIO_CFG;
      }
      return ActStatus::PENDING;
    }

    case ActPhase::BEARER: {
      ATGuard g(60000UL);
      if (!g.ok()) return ActStatus::PENDING;
      if (!activateBearer()) {
        Log::warn(TAG, "cellular.bearer_failed action=radio_reconfig");
        s_actPhase = ActPhase::RADIO_CFG;
        return ActStatus::PENDING;
      }
      _started          = true;
      s_linkUp          = true;
      _lastSignalSample = 0;
      s_recoverQueued.store(CELL_RECOVER_NONE);  // a fresh bearer needs none
      sampleSignalQuality();
      Log::kvf(TAG, "cellular.link.state_change from=down to=up");
      s_actPhase = ActPhase::INIT;
      return ActStatus::DONE;
    }
  }
  return ActStatus::PENDING;  // unreachable - keeps the compiler happy
}

// ---------------------------------------------------------------------------

// Steady-state health probe. Once per HEALTH_PROBE_MS one guarded probe
// checks registration + bearer and samples CSQ. On loss it clears
// _started and CellularModule re-runs activation.
void Cellular::loop() {
  s_driverTask = xTaskGetCurrentTaskHandle();
  if (!_started) return;

  uint32_t now = millis();
  if (s_lastHealthMs != 0 && (now - s_lastHealthMs) < HEALTH_PROBE_MS) return;
  s_lastHealthMs = now;

  ATGuard g;
  if (!g.ok()) {
    Log::warn(TAG, "cellular.health_probe_skipped reason=at_bus_busy");
    return;
  }
  esp_task_wdt_reset();

  // A recycle or reset that a socket failure on another task queued.
  const uint8_t step = cellRecoverPick(s_recoverQueued.exchange(CELL_RECOVER_NONE));
  if (step != CELL_RECOVER_NONE) {
    socketRecoverRun(step);
    if (!_started) return;
  }

  if (!isRegistered() || !isGprsConnectedRaw()) {
    // Hand recovery to CellularModule: ACTIVATING re-walks bring-up one
    // phase per loop under its deadline, so nothing here blocks.
    Log::kvfw(TAG, "cellular.link.state_change from=up to=down reason=network_lost");
    s_linkUp = false;
    for (ModemClient* sk : s_sockets) sk->forget();
    _started = false;
    return;
  }

  esp_task_wdt_reset();
  sampleSignalQuality();
  esp_task_wdt_reset();
}

// ---------------------------------------------------------------------------

// True when activation finished and the last probe saw registration and
// an active bearer: the modem sockets can be expected to carry data.
bool Cellular::connected() {
  return _started && s_linkUp;
}

// ---------------------------------------------------------------------------

// Plain TCP socket on the modem for TLS to ride on. Slot 0 carries the
// MQTT session, slot 1 HTTPS (OTA, net.http).
Client* Cellular::tcpClient(uint8_t slot) {
  return slot < TCP_SLOTS ? s_sockets[slot] : nullptr;
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
// suspended; TCP sockets (and the TLS sessions on them) stay
// registered but cannot push bytes. Issuing CFUN=0 to switch tears the
// TCP session down - the right pattern is just CGNSPWR=1, then on the
// way back CGNSPWR=0 followed by CFUN=1 to wake the LTE data path. The
// broker delivers anything sent during the GNSS window the moment
// CFUN=1 returns.
//
// ATGuard is held across the entire window so the socket clients and
// the health probe cannot race the AT bus. The window is bounded
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
    Log::warn(TAG, "cellular.gps_fix_skipped reason=at_bus_busy");
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
      Shell::pumpConsole();
    }
    modem.disableGPS();
  }

  // Wake the LTE data path. Without this, even with CGNSPWR=0, the modem
  // keeps LTE data suspended and the next socket write silently stalls.
  // CFUN=1 returns OK fast but the bearer needs a short settle, so poll
  // CNACT? until the data context reports active or a short timeout.
  modem.sendAT("+CFUN=1");
  modem.waitResponse(5000UL);

  uint32_t settleEnd = millis() + 5000UL;
  while (millis() < settleEnd && !isGprsConnectedRaw()) {
    delay(250);
    esp_task_wdt_reset();
    Shell::pumpConsole();
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
// monopolising the AT bus. Called from loop() while started.
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

// HTTPS GET over TlsClient on modem socket slot 1. See header for contract.
// TLS runs on the ESP32, so chain and host name are checked the same way
// as on WiFi. The socket takes the AT bus per exchange, never for the
// whole download, so the MQTT session on slot 0 keeps its share.
//
// Status-line + header parsing is line-oriented: collect bytes until
// CRLF, dispatch on the line, switch to body-streaming after the empty
// line separator. Each body chunk reaches `writeCallback` directly so
// the caller can stream to Update.write() / mbedtls_sha256_update()
// without ever buffering the full payload.
bool Cellular::httpsGet(const char* host, const char* path, uint16_t port,
                        const char* caPem,
                        std::function<bool(const uint8_t*, size_t)> writeCallback,
                        int* httpStatus,
                        size_t rangeStart, size_t rangeEndInclusive,
                        uint32_t budgetMs, bool* budgetCut) {
  if (httpStatus) *httpStatus = 0;
  if (budgetCut) *budgetCut = false;
  if (!connected()) {
    Log::warn(TAG, "cellular.https_get_skipped reason=link_down");
    return false;
  }
  // OTA on the loop and net.http on its own task both use slot 1.
  HttpSlotClaim slot(s_httpsBusy);
  if (!slot.held()) {
    Log::warn(TAG, "cellular.https_get_skipped reason=busy");
    return false;
  }

  // The mbedTLS contexts are a few KB, so they live on the heap.
  std::unique_ptr<TlsClient> owner(new (std::nothrow) TlsClient());
  if (!owner) {
    Log::error(TAG, "cellular.https_get_failed reason=alloc");
    return false;
  }
  TlsClient& client = *owner;
  client.setTransport(tcpClient(TCP_SLOT_HTTPS));
  if (caPem && *caPem) {
    client.setCACert(caPem);
  } else {
    Log::kvfw(TAG, "cellular.https_insecure host=%s", host);
    client.setInsecure();
  }

  Log::kvf(TAG, "cellular.https_get_connect host=%s port=%u", host, port);
  s_stats[TCP_SLOT_HTTPS] = {};
  const uint32_t tStart = millis();
  if (!client.connect(host, port)) {
    Log::error(TAG, "cellular.https_get_failed reason=connect");
    return false;
  }

  // Build + send GET. Optional Range header for chunked downloads.
  char rangeHdr[64];
  rangeHdr[0] = '\0';
  if (rangeEndInclusive > 0) {
    snprintf(rangeHdr, sizeof(rangeHdr),
             "Range: bytes=%u-%u\r\n",
             (unsigned)rangeStart, (unsigned)rangeEndInclusive);
  }
  char req[500];
  int reqLen = snprintf(req, sizeof(req),
    "GET %s%s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Connection: close\r\n"
    "User-Agent: thesada-fw\r\n"
    "Accept: */*\r\n"
    "%s"
    "\r\n",
    httpTargetLead(path), path, host, rangeHdr);
  if (reqLen <= 0 || reqLen >= (int)sizeof(req)) {
    Log::error(TAG, "cellular.https_get_failed reason=request_overflow");
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
    if (httpReadBudgetSpent(millis() - tStart, budgetMs)) {
      Log::kvfw(TAG, "cellular.https_get_cut reason=budget budget_ms=%u status=%d body_bytes=%u",
                (unsigned)budgetMs, status, (unsigned)bodyBytes);
      client.stop();
      if (budgetCut) *budgetCut = true;
      return false;
    }
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
        Log::warn(TAG, "cellular.https_get_disconnected stage=headers");
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
        Log::warn(TAG, "cellular.https_get_aborted reason=callback");
        client.stop();
        if (httpStatus) *httpStatus = status;
        return false;
      }
      continue;
    }
    // No bytes this iteration. Decide between wait and exit.
    if (millis() - lastByte > kBodyIdleTimeoutMs) {
      if (contentLen > 0 && bodyBytes < (size_t)contentLen) {
        Log::kvfe(TAG, "cellular.https_get_stall bytes=%u expected=%d idle_ms=%u action=abort",
                  (unsigned)bodyBytes, contentLen,
                  (unsigned)(millis() - lastByte));
      }
      break;
    }
    delay(20);
  }

  client.stop();

  if (status == 0) {
    Log::error(TAG, "cellular.https_get_failed reason=no_status_line");
    return false;
  }
  const SocketStats& st = s_stats[TCP_SLOT_HTTPS];
  Log::kvf(TAG, "cellular.https_get_done status=%d body_bytes=%u content_length=%d ms=%lu reads=%lu read_ms=%lu sock_bytes=%lu polls=%lu avail=%lu",
           status, (unsigned)bodyBytes, contentLen, (unsigned long)(millis() - tStart),
           (unsigned long)st.reads, (unsigned long)st.readMs, (unsigned long)st.readBytes,
           (unsigned long)st.polls, (unsigned long)st.availCalls);
  // Treat short body (got fewer bytes than Content-Length declared) as a
  // transport failure so the OTA caller does not run SHA on a truncated
  // stream and report a hash mismatch. Status code stays correct.
  if (contentLen > 0 && bodyBytes < (size_t)contentLen) return false;
  return true;
}

bool Cellular::httpsBusy() {
  return s_httpsBusy.load() != 0;
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

// Close the sockets and the data context, leaving the modem powered and
// registered. Clears _started so a future re-takeover re-walks
// activation - powerOn() is then a no-op (modem still alive) and the
// BEARER phase re-opens CNACT.
//
// In:  none (AT bus via internal ATGuard)
// Out: CNACT slot 0 closed; modem stays CFUN=1 registered.
void Cellular::dataLinkDown() {
  if (!_modemAlive) return;
  ATGuard g(15000UL);
  if (!g.ok()) {
    Log::warn(TAG, "cellular.data_link_down_skipped reason=at_bus_stuck");
    return;
  }
  for (ModemClient* sk : s_sockets) sk->stop();
  modem.sendAT("+CNACT=0,0");
  modem.waitResponse(5000UL);
  _started = false;
  s_linkUp = false;
  Log::info(TAG, "cellular.data_link_down modem=registered");
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

  // recount 2, per-try timeout 5000 ms.
  char cmd[160];
  int n = snprintf(cmd, sizeof(cmd), "+CDNSGIP=\"%s\",2,5000", host);
  if (n < 0 || (size_t)n >= sizeof(cmd)) {
    Log::kvfw(TAG, "cellular.resolve_skipped reason=host_too_long host_len=%u",
              (unsigned)strlen(host));
    return false;
  }

  ATGuard g(15000UL);
  if (!g.ok()) return false;

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

  // tz 0 -> modem keeps UTC; the ESP32 clock stores a UTC epoch.
  char cmd[128];
  int n = snprintf(cmd, sizeof(cmd), "+CNTP=\"%s\",0", server);
  if (n < 0 || (size_t)n >= sizeof(cmd)) {
    Log::kvfw(TAG, "cellular.ntp_skipped reason=server_too_long server_len=%u",
              (unsigned)strlen(server));
    return false;
  }

  ATGuard g(timeoutMs + 5000UL);
  if (!g.ok()) return false;

  // CNTPCID must be 0 on the SIM7080 (TinyGsmClientSIM7080 notes CID 1
  // does not work on this part).
  cellAT("+CNTPCID=0", "OK", 5000UL, nullptr);

  if (cellAT(cmd, "OK", 5000UL, nullptr) != 1) {
    Log::kvfw(TAG, "cellular.ntp_failed step=config");
    return false;
  }

  // Execute the sync. Result is a +CNTP: <code> URC after the OK; the
  // SIM7080 reports code 1 on success.
  if (cellAT("+CNTP", "OK", 5000UL, nullptr) != 1) {
    Log::kvfw(TAG, "cellular.ntp_failed step=start");
    return false;
  }

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
    Shell::pumpConsole();
  }
  if (code != 1) {
    Log::kvfw(TAG, "cellular.ntp_failed step=result code=%d ms=%lu", code,
              (unsigned long)(millis() - t0));
    return false;
  }

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
  if (!gotClock) {
    Log::kvfw(TAG, "cellular.ntp_failed step=cclk");
    return false;
  }

  // UTC by calendar math, not mktime(): WiFi may have set a TZ via configTime.
  time_t epoch = (time_t)(clockFloorDaysFromCivil(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday) * 86400 +
                          tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec);
  if (epoch < 1700000000L) {
    Log::kvfw(TAG, "cellular.ntp_failed step=epoch epoch=%ld", (long)epoch);
    return false;
  }
  struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  WiFiManager::noteNtpSync();
  return true;
}

#endif // ENABLE_CELLULAR
