// thesada-fw - PowerManager.cpp
// SPDX-License-Identifier: GPL-3.0-only

#include <thesada_config.h>
#ifdef ENABLE_PMU
#include "PowerManager.h"
#include <Config.h>
#include <Log.h>
#include <ModuleRegistry.h>

#include <Wire.h>
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

static const char* TAG = "PowerManager";

// AXP2101 is on the internal board I2C bus (LILYGO T-SIM7080-S3).
// Wire1 is used so this bus does not share state with Wire (used by ADS1115).
static constexpr int PMU_SDA = 15;
static constexpr int PMU_SCL = 7;

static XPowersPMU _pmu;

bool     PowerManager::_pmuOk      = false;

// ---------------------------------------------------------------------------

// Initialize AXP2101 PMU, configure VBUS limits, charging, and ADC channels
void PowerManager::begin() {
  // PMU power config runs unconditionally on every boot.
  // These settings must be applied regardless of whether the heartbeat LED
  // is enabled - without them the AXP2101 will not accept VBUS from a dumb
  // USB charger or solar input and the board cannot power on without a PC.
  Wire1.begin(PMU_SDA, PMU_SCL);
  _pmuOk = _pmu.begin(Wire1, AXP2101_SLAVE_ADDRESS, PMU_SDA, PMU_SCL);

  if (_pmuOk) {
    // Accept VBUS from a dumb USB charger (no data lines / D+D- handshake).
    // Values from the official LILYGO T-SIM7080G examples (MIT licence).
    _pmu.setVbusVoltageLimit(XPOWERS_AXP2101_VBUS_VOL_LIM_4V36);
    _pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_1500MA);

    // Disable TS pin measurement. The TS pin is a battery temperature sensor
    // input that is not connected on this board. If left enabled the PMU
    // treats the absent sensor as a fault and blocks charging.
    _pmu.disableTSPinMeasure();

    // Enable ADC channels for battery and supply monitoring.
    _pmu.enableBattVoltageMeasure();
    _pmu.enableVbusVoltageMeasure();
    _pmu.enableSystemVoltageMeasure();
    _pmu.enableBattDetection();

    _pmu.enableCellbatteryCharge();

    // Charging parameters from config (defaults: 300mA, 4.2V).
    JsonObject cfg = Config::get();
    int chgMa  = cfg["battery"]["charge_ma"]  | 300;
    float chgV = cfg["battery"]["charge_v"]   | 4.2f;

    // Map mA to nearest AXP2101 enum.
    uint8_t curIdx;
    if      (chgMa <= 0)    curIdx = XPOWERS_AXP2101_CHG_CUR_0MA;
    else if (chgMa <= 100)  curIdx = XPOWERS_AXP2101_CHG_CUR_100MA;
    else if (chgMa <= 125)  curIdx = XPOWERS_AXP2101_CHG_CUR_125MA;
    else if (chgMa <= 150)  curIdx = XPOWERS_AXP2101_CHG_CUR_150MA;
    else if (chgMa <= 175)  curIdx = XPOWERS_AXP2101_CHG_CUR_175MA;
    else if (chgMa <= 200)  curIdx = XPOWERS_AXP2101_CHG_CUR_200MA;
    else if (chgMa <= 300)  curIdx = XPOWERS_AXP2101_CHG_CUR_300MA;
    else if (chgMa <= 400)  curIdx = XPOWERS_AXP2101_CHG_CUR_400MA;
    else if (chgMa <= 500)  curIdx = XPOWERS_AXP2101_CHG_CUR_500MA;
    else if (chgMa <= 600)  curIdx = XPOWERS_AXP2101_CHG_CUR_600MA;
    else if (chgMa <= 700)  curIdx = XPOWERS_AXP2101_CHG_CUR_700MA;
    else if (chgMa <= 800)  curIdx = XPOWERS_AXP2101_CHG_CUR_800MA;
    else if (chgMa <= 900)  curIdx = XPOWERS_AXP2101_CHG_CUR_900MA;
    else                    curIdx = XPOWERS_AXP2101_CHG_CUR_1000MA;
    _pmu.setChargerConstantCurr(curIdx);

    // Map voltage to nearest AXP2101 enum.
    uint8_t volIdx;
    if      (chgV <= 4.0f)  volIdx = XPOWERS_AXP2101_CHG_VOL_4V;
    else if (chgV <= 4.1f)  volIdx = XPOWERS_AXP2101_CHG_VOL_4V1;
    else if (chgV <= 4.2f)  volIdx = XPOWERS_AXP2101_CHG_VOL_4V2;
    else if (chgV <= 4.35f) volIdx = XPOWERS_AXP2101_CHG_VOL_4V35;
    else                    volIdx = XPOWERS_AXP2101_CHG_VOL_4V4;
    _pmu.setChargeTargetVoltage(volIdx);

    char chgMsg[64];
    snprintf(chgMsg, sizeof(chgMsg), "PMU ready - charge %dmA / %.1fV cutoff", chgMa, chgV);
    Log::info(TAG, chgMsg);
  } else {
    Log::error(TAG, "PMU init failed");
  }

  // Charging LED - show hardware charge status if heartbeat not using the LED.
  JsonObject cfg = Config::get();
  int32_t hb_s = cfg["device"]["heartbeat_s"] | -1;
  if (hb_s < 0 && _pmuOk) {
    bool chargingLed = cfg["device"]["charging_led"] | true;
    if (chargingLed) {
      _pmu.setChargingLedMode(XPOWERS_CHG_LED_CTRL_CHG);
      Log::info(TAG, "LED: charging indicator");
    } else {
      _pmu.setChargingLedMode(XPOWERS_CHG_LED_OFF);
    }
  }
}

// ---------------------------------------------------------------------------

// No periodic work - placeholder for future use
void PowerManager::loop() {
  // No periodic work - heartbeat moved to core HeartbeatLED.
}

// Turn the PMU charging LED on
void PowerManager::ledOn() {
  if (_pmuOk) _pmu.setChargingLedMode(XPOWERS_CHG_LED_ON);
}

// Turn the PMU charging LED off
void PowerManager::ledOff() {
  if (_pmuOk) _pmu.setChargingLedMode(XPOWERS_CHG_LED_OFF);
}

// ---------------------------------------------------------------------------

// Return true if the PMU was initialized successfully
bool PowerManager::isPmuOk() {
  return _pmuOk;
}

// Return true if a battery is physically connected
bool PowerManager::isBatteryPresent() {
  if (!_pmuOk) return false;
  return _pmu.isBatteryConnect();
}

// Return battery voltage in volts, or 0 if unavailable
float PowerManager::getVoltage() {
  if (!_pmuOk || !_pmu.isBatteryConnect()) return 0.0f;
  return _pmu.getBattVoltage() / 1000.0f;  // mV → V
}

// Return battery charge percentage, or -1 if unavailable
int PowerManager::getPercent() {
  if (!_pmuOk || !_pmu.isBatteryConnect()) return -1;
  return (int)_pmu.getBatteryPercent();
}

// Return true if the battery is currently charging
bool PowerManager::isCharging() {
  if (!_pmuOk) return false;
  return _pmu.isCharging();
}

// Module wrapper for self-registration
class PowerManagerModule : public Module {
public:
  // Delegate to static PowerManager::begin
  void begin() override { PowerManager::begin(); }
  // Delegate to static PowerManager::loop
  void loop() override { PowerManager::loop(); }
  // Return module name
  const char* name() override { return "PowerManager"; }
  // Report PMU and battery status
  void status(ShellOutput out) override {
    char line[96];
    snprintf(line, sizeof(line), "pmu=%s  batt=%s",
             PowerManager::isPmuOk() ? "ok" : "fail",
             PowerManager::isBatteryPresent() ? "yes" : "no");
    out(line);
  }
  // Run PMU self-test
  void selftest(ShellOutput out) override {
    if (!PowerManager::isPmuOk()) { out("[WARN] PMU not available"); return; }
    out("[PASS] PMU ok");
  }
};

MODULE_REGISTER(PowerManagerModule, PRIORITY_POWER)

#endif // ENABLE_PMU
