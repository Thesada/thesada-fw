// thesada-fw - main.cpp
// Boot sequence: Config -> Network -> MQTT -> OTA -> Shell -> modules.
// Network priority: WiFi -> AP fallback.
// If WiFi is down, CellularModule (PRIORITY_NETWORK) handles cellular fallback.
// SPDX-License-Identifier: GPL-3.0-only

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_log.h>
#include <esp_system.h>
#include <Preferences.h>
#include <rom/rtc.h>
#include <soc/soc_caps.h>
#include "thesada_config.h"
#include <Config.h>
#include <ModuleRegistry.h>
#include <WiFiManager.h>
#include <MQTTClient.h>
#include <OTAUpdate.h>
#include <Shell.h>
#include <Log.h>
#include <Console.h>
#include <SleepManager.h>
#include <HeartbeatLED.h>
#ifdef ENABLE_CELLULAR
#include <Cellular.h>
#endif

// Emit a single line at cold boot summarising why the chip restarted.
// Helps debrief outages (see Forgejo #353): pairs esp_reset_reason() with
// per-core rtc_get_reset_reason() (legacy ROM API; covers panic faults
// that the unified API maps to ESP_RST_PANIC) and a persistent brownout
// counter in NVS namespace "boot" key "brownout_n". The counter only
// ever increments, never resets - rolling baseline for field units.
static void logBootCause() {
  esp_reset_reason_t reason = esp_reset_reason();
  const char* reasonStr;
  switch (reason) {
    case ESP_RST_POWERON:   reasonStr = "power_on"; break;
    case ESP_RST_EXT:       reasonStr = "external_pin"; break;
    case ESP_RST_SW:        reasonStr = "sw"; break;
    case ESP_RST_PANIC:     reasonStr = "panic"; break;
    case ESP_RST_INT_WDT:   reasonStr = "int_wdt"; break;
    case ESP_RST_TASK_WDT:  reasonStr = "task_wdt"; break;
    case ESP_RST_WDT:       reasonStr = "wdt"; break;
    case ESP_RST_DEEPSLEEP: reasonStr = "deep_sleep_wake"; break;
    case ESP_RST_BROWNOUT:  reasonStr = "brownout"; break;
    case ESP_RST_SDIO:      reasonStr = "sdio"; break;
    default:                reasonStr = "unknown"; break;
  }

  Preferences p;
  uint32_t brownouts = 0;
  if (p.begin("boot", false)) {
    brownouts = p.getUInt("brownout_n", 0);
    if (reason == ESP_RST_BROWNOUT) {
      brownouts++;
      p.putUInt("brownout_n", brownouts);
    }
    p.end();
  }

  int rtc0 = (int)rtc_get_reset_reason(0);
#if SOC_CPU_CORES_NUM > 1
  int rtc1 = (int)rtc_get_reset_reason(1);
#else
  int rtc1 = -1;
#endif

  char line[160];
  snprintf(line, sizeof(line),
    "reset=%s (%d) rtc_core0=%d rtc_core1=%d brownout_total=%lu",
    reasonStr, (int)reason, rtc0, rtc1, (unsigned long)brownouts);
  Log::info("Boot", line);
}

// Return true if WiFi (or the AP fallback) reports a usable link
static bool networkConnected() {
  return WiFiManager::connected();
}

// Return true if any OTA-capable transport is up. Wider than
// networkConnected(): includes the cellular path so OTAUpdate::loop()
// still ticks when WiFi is down and the modem holds the link.
static bool otaTransportUp() {
  if (networkConnected()) return true;
#ifdef ENABLE_CELLULAR
  if (Cellular::connected()) return true;
#endif
  return false;
}

// Core statics aren't ModuleRegistry modules, so they carry their own
// <key>.enabled gate here (default on). wifi.enabled:false forces cellular.
static bool _wifiEnabled      = true;
static bool _mqttEnabled      = true;
static bool _otaEnabled       = true;
static bool _heartbeatEnabled = true;

void setup() {
  Serial.begin(115200);
  // Don't block waiting for serial - CDC boards hang here without USB host
  uint32_t serialWait = millis();
  while (!Serial && millis() - serialWait < 3000) delay(10);
  Console::begin();

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

  Log::info("Boot", "thesada-fw v" FIRMWARE_VERSION " (" __DATE__ " " __TIME__ ")");
  logBootCause();

  // Quiet the IDF VFS layer's "file does not exist" ERROR logs. Every
  // LittleFS.exists("/foo") for a missing file emits one, even when our
  // application code handles the not-found case immediately. Suppressing
  // the tag turns the noise off without hiding real VFS issues - real
  // mount/IO failures are reported by our own module code.
  esp_log_level_set("vfs_api", ESP_LOG_NONE);

  Config::load();

  {
    JsonObject cfg    = Config::get();
    _wifiEnabled      = cfg["wifi"]["enabled"]      | true;
    _mqttEnabled      = cfg["mqtt"]["enabled"]      | true;
    _otaEnabled       = cfg["ota"]["enabled"]       | true;
    _heartbeatEnabled = cfg["heartbeat"]["enabled"] | true;
    if (!_wifiEnabled) Log::info("Boot", "wifi.enabled=false - WiFi skipped, cellular is primary transport");
  }

  // Bring WiFi up. CellularModule registers later via PRIORITY_NETWORK
  // and handles the fallback path if WiFi never associates.
  if (_wifiEnabled && !networkConnected()) {
    WiFiManager::begin();
  }

  if (_otaEnabled && networkConnected()) {
    // OTA check BEFORE MQTT - heap is still contiguous here (~80KB max alloc).
    // After MQTT + modules load, heap fragments and a second TLS session
    // for the OTA manifest fetch can't allocate on WROOM-32 boards.
    // If an update is found, the device flashes and reboots - never reaches MQTT.
    OTAUpdate::begin();
    OTAUpdate::checkNow();  // immediate check while heap is clean
  }
  // Registers the subscription set that Cellular::smsubAll mirrors onto the
  // cellular MQTT session on failover. connect() is a no-op if WiFi is down.
  if (_mqttEnabled) MQTTClient::begin();

  Shell::begin();
  if (_heartbeatEnabled) HeartbeatLED::begin();

  // All modules self-registered via MODULE_REGISTER. beginAll() sorts by priority.
  ModuleRegistry::beginAll();

  SleepManager::begin();

  Log::info("Boot", "Ready. Type 'help' for commands.");
}

void loop() {
  esp_task_wdt_reset();

  // Serial shell - drain console input. Same helper is reused inside
  // long-running blocking loops (e.g. cellular registration polling) so
  // the shell stays interactive during failover bring-up.
  Shell::pumpConsole();

  // WiFi management runs even with Ethernet - keeps fallback ready
  if (_wifiEnabled) WiFiManager::loop();

  // MQTTClient::loop runs on every tick regardless of WiFi/Eth state.
  // The heap-stats trigger and other transport-agnostic work inside it
  // need to fire when cellular fallback is active even though
  // networkConnected() (WiFi/Eth-only) is false. The WiFi-specific
  // reconnect path inside loop() is no-op when WiFi is down.
  if (_mqttEnabled) MQTTClient::loop();
  if (_otaEnabled && otaTransportUp()) {
    OTAUpdate::loop();
  }

  // Drain the deferred Shell ring. Commands enqueued from non-main
  // task contexts (WS serial, future BLE/Telegram CLIs) execute here with
  // full main-loop stack instead of inside an AsyncTCP / mbedtls callback.
  Shell::loop();

  if (_heartbeatEnabled) HeartbeatLED::loop();
  ModuleRegistry::loopAll();
  SleepManager::loop();
}
