// thesada-fw - main.cpp
// Boot sequence: Config -> Network -> MQTT -> OTA -> Shell -> modules.
// Network priority: Ethernet (if ENABLE_ETH) -> WiFi -> AP fallback.
// If all fail, CellularModule (PRIORITY_NETWORK) handles cellular fallback.
// SPDX-License-Identifier: GPL-3.0-only

#include <Arduino.h>
#include <esp_task_wdt.h>
#include "thesada_config.h"
#include <Config.h>
#include <ModuleRegistry.h>
#include <WiFiManager.h>
#include <MQTTClient.h>
#include <OTAUpdate.h>
#include <Shell.h>
#include <SleepManager.h>
#include <HeartbeatLED.h>
#ifdef ENABLE_ETH
#include <EthModule.h>
#endif

// Serial line buffer for the shell.
static char _serialBuf[256];
static uint8_t _serialPos = 0;

// Serial output callback for the shell
static void serialOut(const char* line) {
  Serial.println(line);
}

// Return true if any network transport is connected
static bool networkConnected() {
#ifdef ENABLE_ETH
  if (EthModule::connected()) return true;
#endif
  return WiFiManager::connected();
}

void setup() {
  Serial.begin(115200);
  // Don't block waiting for serial - CDC boards hang here without USB host
  uint32_t serialWait = millis();
  while (!Serial && millis() - serialWait < 3000) delay(10);

  // Hardware watchdog: reboot if loop() doesn't feed within 30s.
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);

  Serial.println("[INF][Boot] thesada-fw v" FIRMWARE_VERSION " (" __DATE__ " " __TIME__ ")");

  Config::load();

  // Network priority: Ethernet -> WiFi -> AP fallback.
#ifdef ENABLE_ETH
  EthModule::earlyInit();
#endif

  if (!networkConnected()) {
    WiFiManager::begin();
  }

  if (networkConnected()) {
    MQTTClient::begin();
    OTAUpdate::begin();
  }

  Shell::begin();
  HeartbeatLED::begin();

  // All modules self-registered via MODULE_REGISTER. beginAll() sorts by priority.
  ModuleRegistry::beginAll();

  SleepManager::begin();

  Serial.println("[INF][Boot] Ready. Type 'help' for commands.");
}

void loop() {
  esp_task_wdt_reset();

  // Serial shell - read characters, execute on newline.
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      _serialBuf[_serialPos] = '\0';
      if (_serialPos > 0) {
        Shell::execute(_serialBuf, serialOut);
      }
      _serialPos = 0;
    } else if (_serialPos < sizeof(_serialBuf) - 1) {
      _serialBuf[_serialPos++] = c;
    }
  }

  // WiFi management runs even with Ethernet - keeps fallback ready
  WiFiManager::loop();

  if (networkConnected()) {
    MQTTClient::loop();
    OTAUpdate::loop();
  }

  HeartbeatLED::loop();
  ModuleRegistry::loopAll();
  SleepManager::loop();
}
