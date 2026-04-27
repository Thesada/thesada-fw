// thesada-fw - Cellular.cpp
// SIM7080G Cat-M1 with modem-native MQTT over TLS (AT+SMCONF/SMCONN/SMPUB).
// SPDX-License-Identifier: GPL-3.0-only

#include <thesada_config.h>
#ifdef ENABLE_CELLULAR

#include "Cellular.h"
#include <Config.h>
#include <Log.h>
#include <LittleFS.h>

// ── TinyGSM ─────────────────────────────────────────────────────────────────
// TINY_GSM_MODEM_SIM7080 and TINY_GSM_RX_BUFFER=1024 set via build_flags.
#include <TinyGsmClient.h>

// ── XPowersLib (AXP2101 PMU) ─────────────────────────────────────────────────
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

static const char* TAG = "Cellular";

// ── Board pins (LILYGO T-SIM7080-S3) ─────────────────────────────────────────
#ifdef BOARD_LILYGO_T_SIM7080_S3
  static constexpr int PIN_MODEM_RXD = 4;
  static constexpr int PIN_MODEM_TXD = 5;
  static constexpr int PIN_MODEM_PWR = 41;
  static constexpr int PIN_MODEM_DTR = 42;
  static constexpr int PIN_I2C_SDA   = 15;
  static constexpr int PIN_I2C_SCL   = 7;
#else
  #error "No board defined - set BOARD_LILYGO_T_SIM7080_S3 in config.h"
#endif


// ── Module state ─────────────────────────────────────────────────────────────
bool     Cellular::_started        = false;
bool     Cellular::_mqttConnected  = false;
bool     Cellular::_publishGate    = false;
bool     Cellular::_hasCACert      = false;
int      Cellular::_signalQuality  = 99;
uint32_t Cellular::_lastSignalSample = 0;

// Serial port for TinyGSM
static TinyGsm modem(Serial1);

// Sample AT+CSQ at most every 30 s to keep AT bus clean.
static constexpr uint32_t SIGNAL_SAMPLE_MS = 30UL * 1000UL;

// ---------------------------------------------------------------------------

// Configure AXP2101 PMU power rails for the modem
void Cellular::initPMU() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  XPowersPMU pmu;
  if (!pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, PIN_I2C_SDA, PIN_I2C_SCL)) {
    Log::error(TAG, "PMU init failed");
    return;
  }
  // Power off DC3 briefly if cold boot to ensure clean modem Vcc.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
    pmu.disableDC3();
    delay(300);
  }
  pmu.setDC3Voltage(3000);   // modem Vcc 3.0 V
  pmu.enableDC3();
  pmu.setBLDO2Voltage(3300); // antenna rail 3.3 V
  pmu.enableBLDO2();
  pmu.disableTSPinMeasure();
  Log::info(TAG, "PMU ready");
}

// ---------------------------------------------------------------------------

// Start serial and pulse PWRKEY until the modem responds to AT
void Cellular::wakeModem() {
  Serial1.begin(115200, SERIAL_8N1, PIN_MODEM_RXD, PIN_MODEM_TXD);
  pinMode(PIN_MODEM_PWR, OUTPUT);
  pinMode(PIN_MODEM_DTR, OUTPUT);

  int retry = 0;
  while (!modem.testAT(1000)) {
    if (++retry > 6) {
      Log::info(TAG, "PWRKEY pulse");
      digitalWrite(PIN_MODEM_PWR, LOW);  delay(100);
      digitalWrite(PIN_MODEM_PWR, HIGH); delay(1000);
      digitalWrite(PIN_MODEM_PWR, LOW);
      retry = 0;
    }
  }
  Log::info(TAG, "Modem alive");
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
    modem.waitResponse(30000UL, "DOWNLOAD");
    modem.stream.write(cert + written, chunk);
    if (modem.waitResponse(30000UL) == 1) {
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
  modem.waitResponse(20000UL);
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
  modem.waitResponse(20000UL);

  // Critical: let LTE-M radio settle before polling registration.
  char msg[64];
  snprintf(msg, sizeof(msg), "RF settle %lu ms...", rfSettleMs);
  Log::info(TAG, msg);
  delay(rfSettleMs);

  Log::info(TAG, "Waiting for registration...");
  SIM70xxRegStatus s;
  uint32_t start = millis();
  do {
    s = modem.getRegistrationStatus();
    if (s == REG_OK_HOME)    { Log::info(TAG, "Registered - HOME");    break; }
    if (s == REG_OK_ROAMING) { Log::info(TAG, "Registered - ROAMING"); break; }
    if (s == REG_DENIED)     { Log::error(TAG, "Registration DENIED"); return false; }
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
    if (modem.waitResponse(30000UL) != 1) {
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
  const char* broker   = cfg["mqtt"]["broker"]   | "mqtt.thesada.app";
  int         port     = cfg["mqtt"]["port"]      | 8883;
  const char* user     = cfg["mqtt"]["user"]      | "";
  const char* password = cfg["mqtt"]["password"]  | "";
  const char* devName  = cfg["device"]["name"]    | "thesada-node";

  Log::info(TAG, "Configuring MQTT...");

  modem.sendAT("+SMDISC");
  modem.waitResponse(5000UL);

  modem.sendAT("+SMCONF=\"URL\",", broker, ",", port);
  if (modem.waitResponse() != 1) { Log::error(TAG, "MQTT URL failed"); return false; }

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
  if (modem.waitResponse(30000UL) != 1) {
    Log::error(TAG, "MQTT connect failed");
    return false;
  }

  Log::info(TAG, "MQTT connected");
  return true;
}

// ---------------------------------------------------------------------------

// Check if the modem is registered on the cellular network
bool Cellular::isRegistered() {
  SIM70xxRegStatus s = modem.getRegistrationStatus();
  return (s == REG_OK_HOME || s == REG_OK_ROAMING);
}

// Query the modem MQTT connection state via AT command
bool Cellular::mqttIsConnected() {
  modem.sendAT("+SMSTATE?");
  if (modem.waitResponse(3000UL, "+SMSTATE: ") == 1) {
    String res = modem.stream.readStringUntil('\r');
    return res.toInt() == 1;
  }
  return false;
}

// ---------------------------------------------------------------------------

// Initialize PMU, modem, SIM, CA cert, network, and MQTT connection
void Cellular::begin() {
  if (_started) return;  // already initialised - modem is up, only loop() needed
  Log::info(TAG, "Starting cellular path...");

  initPMU();
  wakeModem();

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
    delay(10000);
  }
  while (!mqttConnect()) {
    Log::warn(TAG, "MQTT connect failed, retry in 10s");
    delay(10000);
  }

  _started          = true;
  _mqttConnected    = true;
  _publishGate      = true;
  _lastSignalSample = 0;
  sampleSignalQuality();
}

// ---------------------------------------------------------------------------

// Cellular-only recovery: re-register on network loss, reconnect MQTT on drop.
// WiFi-vs-cellular policy is handled by CellularModule, not here.
void Cellular::loop() {
  if (!_started) return;

  // Recovery: network dropped.
  if (!isRegistered() || !modem.isGprsConnected()) {
    Log::warn(TAG, "Network lost - re-registering");
    _mqttConnected = false;
    while (!networkConnect()) {
      Log::warn(TAG, "Retry network in 10s");
      delay(10000);
    }
    while (!mqttConnect()) {
      Log::warn(TAG, "Retry MQTT in 10s");
      delay(10000);
    }
    _mqttConnected = true;
    return;
  }

  // Recovery: MQTT dropped, network still up.
  if (!mqttIsConnected()) {
    Log::warn(TAG, "MQTT dropped - reconnecting");
    _mqttConnected = false;
    if (mqttConnect()) {
      _mqttConnected = true;
    } else {
      delay(5000);
    }
  }

  sampleSignalQuality();
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

// Refresh _signalQuality from AT+CSQ at most every SIGNAL_SAMPLE_MS to avoid
// monopolising the AT bus. Called from begin() and loop() while started.
void Cellular::sampleSignalQuality() {
  uint32_t now = millis();
  if (_lastSignalSample != 0 && (now - _lastSignalSample) < SIGNAL_SAMPLE_MS) return;
  _lastSignalSample = now;
  int rssi = modem.getSignalQuality();  // TinyGSM: 0..31, 99 = unknown
  _signalQuality = rssi;
}

// ---------------------------------------------------------------------------

// Publish a message via modem-native MQTT (AT+SMPUB)
bool Cellular::publish(const char* topic, const char* payload) {
  if (!connected()) return false;

  size_t len = strlen(payload);
  char cmd[128];
  snprintf(cmd, sizeof(cmd), "+SMPUB=\"%s\",%d,1,0", topic, (int)len);

  modem.sendAT(cmd);
  if (modem.waitResponse(5000UL, ">") != 1) {
    Log::warn(TAG, "No SMPUB prompt");
    _mqttConnected = false;
    return false;
  }

  modem.stream.write(payload, len);

  if (modem.waitResponse(5000UL) == 1) {
    return true;
  }
  Log::warn(TAG, "SMPUB failed");
  _mqttConnected = false;
  return false;
}

#endif // ENABLE_CELLULAR
