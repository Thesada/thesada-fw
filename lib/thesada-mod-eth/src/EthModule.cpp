// thesada-fw - EthModule.cpp
// LAN8720A Ethernet via ESP32 RMII interface.
// Provides network connectivity as an alternative to WiFi.
// Config: eth.phy_addr, eth.power_pin, eth.mdc, eth.mdio
// WT32-ETH01 defaults: phy_addr=1, power_pin=16, mdc=23, mdio=18, clk=GPIO0_IN
// SPDX-License-Identifier: GPL-3.0-only

#include <thesada_config.h>
#ifdef ENABLE_ETH

#include "EthModule.h"
#include <Config.h>
#include <Log.h>
#include <Shell.h>
#include <ModuleRegistry.h>
#include <ETH.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_task_wdt.h>

static const char* TAG = "ETH";

bool EthModule::_connected = false;

// Ethernet event handler. Callback signature changed in arduino-esp32 3.x:
// 2.x: void(WiFiEvent_t). 3.x: void(arduino_event_id_t, arduino_event_info_t).
#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onEthEvent(arduino_event_id_t event, arduino_event_info_t /*info*/) {
#else
static void onEthEvent(WiFiEvent_t event) {
#endif
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Log::info(TAG, "Started");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Log::info(TAG, "Link up");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP: {
      char msg[96];
      snprintf(msg, sizeof(msg), "IP: %s  MAC: %s  %s %dMbps",
               ETH.localIP().toString().c_str(),
               ETH.macAddress().c_str(),
               ETH.fullDuplex() ? "full-duplex" : "half-duplex",
               ETH.linkSpeed());
      Log::info(TAG, msg);
      EthModule::_connected = true;
      break;
    }
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Log::warn(TAG, "Disconnected");
      EthModule::_connected = false;
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Log::info(TAG, "Stopped");
      EthModule::_connected = false;
      break;
    default:
      break;
  }
}

// Early init - called from main.cpp before WiFiManager.
// Sets up the PHY and waits briefly for link. If Ethernet connects,
// WiFiManager::begin() is skipped entirely.
void EthModule::earlyInit() {
  JsonObject cfg = Config::get();

  uint8_t phyAddr  = cfg["eth"]["phy_addr"]  | 1;
  int     powerPin = cfg["eth"]["power_pin"] | 16;
  int     mdc      = cfg["eth"]["mdc"]       | 23;
  int     mdio     = cfg["eth"]["mdio"]      | 18;

  WiFi.onEvent(onEthEvent);

  Log::info(TAG, "Initializing LAN8720A...");
  // ETH.begin signature changed in arduino-esp32 3.x: type first, power last.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!ETH.begin(ETH_PHY_LAN8720, phyAddr, mdc, mdio, powerPin, ETH_CLOCK_GPIO0_IN)) {
#else
  if (!ETH.begin(phyAddr, powerPin, mdc, mdio, ETH_PHY_LAN8720, ETH_CLOCK_GPIO0_IN)) {
#endif
    Log::error(TAG, "ETH.begin() failed");
    return;
  }

  // Optional static IP
  const char* staticIp = cfg["eth"]["static_ip"] | "";
  if (strlen(staticIp) > 0) {
    IPAddress ip, gw, subnet, dns;
    ip.fromString(staticIp);
    gw.fromString(cfg["eth"]["gateway"] | "192.168.1.1");
    subnet.fromString(cfg["eth"]["subnet"] | "255.255.255.0");
    dns.fromString(cfg["eth"]["dns"] | "8.8.8.8");
    ETH.config(ip, gw, subnet, dns);
    char msg[64];
    snprintf(msg, sizeof(msg), "Static IP: %s", staticIp);
    Log::info(TAG, msg);
  }

  // Wait for link + DHCP (up to 5s), feed watchdog while waiting
  uint32_t t0 = millis();
  while (!_connected && millis() - t0 < 5000) {
    esp_task_wdt_reset();
    delay(100);
  }

  if (_connected) {
    // Start NTP over Ethernet
    const char* ntpSrv = cfg["ntp"]["server"]  | "pool.ntp.org";
    const char* ntpFb  = cfg["ntp"]["server2"] | "time.cloudflare.com";
    int32_t tzOff      = cfg["ntp"]["tz_offset_s"] | 0;
    configTime(tzOff, 0, ntpSrv, ntpFb);
    uint32_t ntpStart = millis();
    while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED &&
           millis() - ntpStart < 10000) {
      esp_task_wdt_reset();
      delay(200);
    }
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      Log::info(TAG, "NTP synced");
    } else {
      Log::warn(TAG, "NTP pending - will sync in background");
    }
  } else {
    Log::warn(TAG, "No Ethernet link - WiFi fallback");
  }
}

// Module begin - registers shell command, skips PHY init (already done in earlyInit)
void EthModule::begin() {
  // Register shell command
  Shell::registerCommand("net.eth", "Ethernet connection status",
      [](int argc, char** argv, ShellOutput out) {
        char line[128];
        snprintf(line, sizeof(line), "ETH: %s", EthModule::connected() ? "connected" : "disconnected");
        out(line);
        if (EthModule::connected()) {
          snprintf(line, sizeof(line), "  IP:    %s", ETH.localIP().toString().c_str());
          out(line);
          snprintf(line, sizeof(line), "  MAC:   %s", ETH.macAddress().c_str());
          out(line);
          snprintf(line, sizeof(line), "  Speed: %dMbps %s", ETH.linkSpeed(),
                   ETH.fullDuplex() ? "full-duplex" : "half-duplex");
          out(line);
        }
      });

  Log::info(TAG, _connected ? "Ready - Ethernet active" : "Ready - waiting for link");
}

// No periodic work - events handle link changes
void EthModule::loop() {}

// Return Ethernet connection state
bool EthModule::connected() { return _connected; }

// Return local IP as string
String EthModule::localIP() {
  return _connected ? ETH.localIP().toString() : String("0.0.0.0");
}

// Report Ethernet status
void EthModule::status(ShellOutput out) {
  char line[96];
  if (_connected) {
    snprintf(line, sizeof(line), "connected  %s  %dMbps",
             ETH.localIP().toString().c_str(), ETH.linkSpeed());
  } else {
    snprintf(line, sizeof(line), "disconnected");
  }
  out(line);
}

// Run Ethernet self-test
void EthModule::selftest(ShellOutput out) {
  if (_connected) {
    char line[64];
    snprintf(line, sizeof(line), "[PASS] Ethernet: %s %dMbps",
             ETH.localIP().toString().c_str(), ETH.linkSpeed());
    out(line);
  } else {
    out("[FAIL] Ethernet not connected");
  }
}

MODULE_REGISTER(EthModule, PRIORITY_NETWORK)

#endif // ENABLE_ETH
