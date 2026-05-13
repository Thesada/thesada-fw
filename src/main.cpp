// thesada-fw - main.cpp
// Boot sequence: Config -> Network -> MQTT -> OTA -> Shell -> modules.
// Network priority: WiFi -> AP fallback.
// If WiFi is down, CellularModule (PRIORITY_NETWORK) handles cellular fallback.
// SPDX-License-Identifier: GPL-3.0-only

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_log.h>
#include "thesada_config.h"
#include <Config.h>
#include <ModuleRegistry.h>
#include <WiFiManager.h>
#include <MQTTClient.h>
#include <OTAUpdate.h>
#include <Shell.h>
#include <SleepManager.h>
#include <HeartbeatLED.h>
#ifdef ENABLE_CELLULAR
#include <Cellular.h>
#endif

// Return true if WiFi (or the AP fallback) reports a usable link
static bool networkConnected() {
  return WiFiManager::connected();
}

// Return true if any OTA-capable transport is up. Wider than
// networkConnected(): includes the cellular path so OTAUpdate::loop()
// still ticks when WiFi is down and the modem holds the link (#220).
static bool otaTransportUp() {
  if (networkConnected()) return true;
#ifdef ENABLE_CELLULAR
  if (Cellular::connected()) return true;
#endif
  return false;
}

void setup() {
  Serial.begin(115200);
  // Don't block waiting for serial - CDC boards hang here without USB host
  uint32_t serialWait = millis();
  while (!Serial && millis() - serialWait < 3000) delay(10);

  // Hardware watchdog: reboot if loop() doesn't feed within 30s.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  // IDF 5 pre-initializes TWDT. Use reconfigure to extend timeout to 30s.
  esp_task_wdt_config_t wdt_cfg = {
    .timeout_ms = 60000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  esp_task_wdt_reconfigure(&wdt_cfg);
#else
  esp_task_wdt_init(30, true);
#endif
  esp_task_wdt_add(NULL);

  Serial.println("[INF][Boot] thesada-fw v" FIRMWARE_VERSION " (" __DATE__ " " __TIME__ ")");

  // Quiet the IDF VFS layer's "file does not exist" ERROR logs. Every
  // LittleFS.exists("/foo") for a missing file emits one, even when our
  // application code handles the not-found case immediately. Suppressing
  // the tag turns the noise off without hiding real VFS issues - real
  // mount/IO failures are reported by our own module code.
  esp_log_level_set("vfs_api", ESP_LOG_NONE);

  Config::load();

  // Bring WiFi up. CellularModule registers later via PRIORITY_NETWORK
  // and handles the fallback path if WiFi never associates.
  if (!networkConnected()) {
    WiFiManager::begin();
  }

  if (networkConnected()) {
    // OTA check BEFORE MQTT - heap is still contiguous here (~80KB max alloc).
    // After MQTT + modules load, heap fragments and a second TLS session
    // for the OTA manifest fetch can't allocate on WROOM-32 boards.
    // If an update is found, the device flashes and reboots - never reaches MQTT.
    OTAUpdate::begin();
    OTAUpdate::checkNow();  // immediate check while heap is clean
  }
  // MQTTClient::begin runs unconditionally. It registers the subscription
  // set (cli/#, cmd/lua/reload, etc) and starts the subscription registry
  // that Cellular::smsubAll mirrors onto the cellular MQTT session when
  // the failover path activates. With this gated on WiFi, a WiFi-down boot
  // would leave the subscription list empty and cellular MQTT inbound
  // would have nothing to dispatch.
  // connect() is a no-op if WiFi is down; loop() retries.
  MQTTClient::begin();

  Shell::begin();
  HeartbeatLED::begin();

  // All modules self-registered via MODULE_REGISTER. beginAll() sorts by priority.
  ModuleRegistry::beginAll();

  SleepManager::begin();

  Serial.println("[INF][Boot] Ready. Type 'help' for commands.");
}

void loop() {
  esp_task_wdt_reset();

  // Serial shell - drain console input. Same helper is reused inside
  // long-running blocking loops (e.g. cellular registration polling) so
  // the shell stays interactive during failover bring-up.
  Shell::pumpConsole();

  // WiFi management runs even with Ethernet - keeps fallback ready
  WiFiManager::loop();

  // MQTTClient::loop runs on every tick regardless of WiFi/Eth state.
  // The heap-stats trigger and other transport-agnostic work inside it
  // need to fire when cellular fallback is active even though
  // networkConnected() (WiFi/Eth-only) is false. The WiFi-specific
  // reconnect path inside loop() is no-op when WiFi is down.
  MQTTClient::loop();
  if (otaTransportUp()) {
    OTAUpdate::loop();
  }

  // Drain the deferred Shell ring. Commands enqueued from non-main
  // task contexts (WS serial, future BLE/Telegram CLIs) execute here with
  // full main-loop stack instead of inside an AsyncTCP / mbedtls callback.
  Shell::loop();

  HeartbeatLED::loop();
  ModuleRegistry::loopAll();
  SleepManager::loop();
}
