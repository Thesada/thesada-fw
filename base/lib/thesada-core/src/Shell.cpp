// thesada-fw - Shell.cpp
// Interactive CLI shell. All commands output through a callback so the
// same implementation works for serial and WebSocket transports.
// SPDX-License-Identifier: GPL-3.0-only

#include "Shell.h"
#include "Config.h"
#include "Log.h"
#include "WiFiManager.h"
#include "MQTTClient.h"
#include "ModuleRegistry.h"
#include "OTAUpdate.h"
#include <thesada_config.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_system.h>
#include <esp_app_format.h>

#include <LittleFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <sys/time.h>

ShellEntry Shell::_commands[MAX_COMMANDS];
int Shell::_commandCount = 0;
char Shell::_parseBuf[256];

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

// Tokenize a command line into argv-style arguments
int Shell::parse(const char* line, char** argv, int maxArgs) {
  strncpy(_parseBuf, line, sizeof(_parseBuf) - 1);
  _parseBuf[sizeof(_parseBuf) - 1] = '\0';

  int argc = 0;
  char* token = strtok(_parseBuf, " \t");
  while (token && argc < maxArgs) {
    argv[argc++] = token;
    token = strtok(nullptr, " \t");
  }
  return argc;
}

// ---------------------------------------------------------------------------
// Command registry
// ---------------------------------------------------------------------------

// Register a named command with help text and handler
void Shell::registerCommand(const char* name, const char* help, ShellCommand handler) {
  if (_commandCount >= MAX_COMMANDS) return;
  _commands[_commandCount] = {name, help, handler};
  _commandCount++;
}

// Parse and dispatch a command line to the matching handler
void Shell::execute(const char* line, ShellOutput out) {
  if (!line || strlen(line) == 0) return;

  char* argv[16];
  int argc = parse(line, argv, 16);
  if (argc == 0) return;

  for (int i = 0; i < _commandCount; i++) {
    if (strcasecmp(argv[0], _commands[i].name) == 0) {
      _commands[i].handler(argc, argv, out);
      return;
    }
  }

  char msg[128];
  snprintf(msg, sizeof(msg), "Unknown command: %s  (type 'help')", argv[0]);
  out(msg);
}

// Print all registered commands with their help text
void Shell::listCommands(ShellOutput out) {
  for (int i = 0; i < _commandCount; i++) {
    char line[128];
    snprintf(line, sizeof(line), "  %-16s %s", _commands[i].name, _commands[i].help);
    out(line);
  }
}

// ---------------------------------------------------------------------------
// Filesystem commands
// ---------------------------------------------------------------------------

// All filesystem commands use LittleFS by default.
// SD card paths (/sd/*) are handled by SDModule which registers
// its own fs.sd.ls, fs.sd.cat etc commands in begin().
static FS* resolveFS(const char* /*path*/) {
  return &LittleFS;
}

// Strip path prefix (no-op now, kept for compatibility)
static const char* stripPrefix(const char* path) {
  return path;
}

// List files in a directory
static void cmd_ls(int argc, char** argv, ShellOutput out) {
  const char* path = (argc > 1) ? argv[1] : "/";
  FS* fs = resolveFS(path);
  const char* fsPath = stripPrefix(path);

  File dir = fs->open(fsPath);
  if (!dir || !dir.isDirectory()) {
    out("Not a directory or not found");
    return;
  }

  char line[128];
  File entry = dir.openNextFile();
  while (entry) {
    if (entry.isDirectory()) {
      snprintf(line, sizeof(line), "  [DIR]  %s/", entry.name());
    } else {
      snprintf(line, sizeof(line), "  %6d  %s", (int)entry.size(), entry.name());
    }
    out(line);
    entry = dir.openNextFile();
  }
}

// Print file contents line by line
static void cmd_cat(int argc, char** argv, ShellOutput out) {
  if (argc < 2) { out("Usage: cat <path>"); return; }
  FS* fs = resolveFS(argv[1]);
  const char* fsPath = stripPrefix(argv[1]);

  File f = fs->open(fsPath, "r");
  if (!f) { out("File not found"); return; }

  // Read line by line to avoid huge memory allocation.
  while (f.available()) {
    String line = f.readStringUntil('\n');
    out(line.c_str());
  }
  f.close();
}

// Remove a file from the filesystem
static void cmd_rm(int argc, char** argv, ShellOutput out) {
  if (argc < 2) { out("Usage: rm <path>"); return; }
  FS* fs = resolveFS(argv[1]);
  const char* fsPath = stripPrefix(argv[1]);

  if (fs->remove(fsPath)) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Removed %s", fsPath);
    out(msg);
  } else {
    out("Failed to remove (file not found or directory)");
  }
}

// Write content to a file
static void cmd_write(int argc, char** argv, ShellOutput out) {
  // write <path> <content...>
  if (argc < 3) { out("Usage: write <path> <content...>"); return; }
  FS* fs = resolveFS(argv[1]);
  const char* fsPath = stripPrefix(argv[1]);

  // Reconstruct content from remaining args.
  String content;
  for (int i = 2; i < argc; i++) {
    if (i > 2) content += " ";
    content += argv[i];
  }

  File f = fs->open(fsPath, "w");
  if (!f) { out("Failed to open for writing"); return; }
  f.print(content);
  f.close();

  char msg[64];
  snprintf(msg, sizeof(msg), "Wrote %d bytes to %s", content.length(), fsPath);
  out(msg);
}

// Rename or move a file
static void cmd_mv(int argc, char** argv, ShellOutput out) {
  if (argc < 3) { out("Usage: mv <src> <dst>"); return; }
  FS* fs = resolveFS(argv[1]);
  const char* src = stripPrefix(argv[1]);
  const char* dst = stripPrefix(argv[2]);

  if (fs->rename(src, dst)) {
    char msg[128];
    snprintf(msg, sizeof(msg), "Renamed %s -> %s", src, dst);
    out(msg);
  } else {
    out("Failed to rename");
  }
}

// Show disk usage for LittleFS and SD card
static void cmd_df(int argc, char** argv, ShellOutput out) {
  char line[128];

  size_t total = LittleFS.totalBytes();
  size_t used  = LittleFS.usedBytes();
  snprintf(line, sizeof(line), "LittleFS: %d / %d bytes used (%d%% free)",
           (int)used, (int)total, (int)((total - used) * 100 / total));
  out(line);

  // SD card info is reported by SDModule::status() via module.status command
}

// ---------------------------------------------------------------------------
// Config commands
// ---------------------------------------------------------------------------

// Read a config value by dot-notation key
static void cmd_config_get(int argc, char** argv, ShellOutput out) {
  if (argc < 2) { out("Usage: config.get <key>  (dot notation, e.g. mqtt.broker)"); return; }

  JsonObject cfg = Config::get();
  JsonVariant current = cfg;

  char key[128];
  strncpy(key, argv[1], sizeof(key) - 1);
  key[sizeof(key) - 1] = '\0';

  char* token = strtok(key, ".");
  while (token) {
    if (current.is<JsonObject>()) {
      current = current[token];
    } else {
      out("Key not found");
      return;
    }
    token = strtok(nullptr, ".");
  }

  if (current.isNull()) {
    out("null");
  } else {
    String val;
    serializeJsonPretty(current, val);
    out(val.c_str());
  }
}

// Set a config value by dot-notation key and save to flash
static void cmd_config_set(int argc, char** argv, ShellOutput out) {
  // config.set <key> <value>
  // This modifies the in-memory config. Use config.save to persist.
  if (argc < 3) { out("Usage: config.set <key> <value>  (then config.save to persist)"); return; }

  // For now, only support simple top-level.section.key paths.
  // Reconstruct value from remaining args.
  String value;
  for (int i = 2; i < argc; i++) {
    if (i > 2) value += " ";
    value += argv[i];
  }

  // Read current config file, parse, modify, and update in-memory.
  if (!LittleFS.exists("/config.json")) { out("config.json not found"); return; }
  File f = LittleFS.open("/config.json", "r");
  if (!f) { out("Failed to read config.json"); return; }
  String json = f.readString();
  f.close();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) { out("JSON parse error in config.json"); return; }

  // Walk the dot path and set the value.
  char key[128];
  strncpy(key, argv[1], sizeof(key) - 1);
  key[sizeof(key) - 1] = '\0';

  // Split into parent path + final key.
  char* lastDot = strrchr(key, '.');
  if (lastDot) {
    *lastDot = '\0';
    const char* finalKey = lastDot + 1;

    JsonVariant parent = doc.as<JsonVariant>();
    char* token = strtok(key, ".");
    while (token) {
      if (parent.is<JsonObject>()) {
        parent = parent[token];
      } else {
        out("Parent key not found");
        return;
      }
      token = strtok(nullptr, ".");
    }

    if (!parent.is<JsonObject>()) { out("Parent is not an object"); return; }

    // Try to detect type: number, boolean, or string.
    if (value == "true") parent[finalKey] = true;
    else if (value == "false") parent[finalKey] = false;
    else {
      char* end;
      long lv = strtol(value.c_str(), &end, 10);
      if (*end == '\0' && value.length() > 0) {
        parent[finalKey] = lv;
      } else {
        float fv = strtof(value.c_str(), &end);
        if (*end == '\0' && value.length() > 0) {
          parent[finalKey] = fv;
        } else {
          parent[finalKey] = value;
        }
      }
    }
  } else {
    // Top-level key.
    out("Cannot set top-level keys directly (use section.key format)");
    return;
  }

  // Write back to LittleFS.
  File wf = LittleFS.open("/config.json", "w");
  if (!wf) { out("Failed to write config.json"); return; }
  serializeJsonPretty(doc, wf);
  wf.close();

  // Config saved to flash. Reload with config.reload to apply.
  // (Not auto-reloading here because Config::load() reinitializes MQTT,
  //  which would drop the connection before the response is sent over MQTT CLI.)

  char msg[128];
  snprintf(msg, sizeof(msg), "Set %s = %s (saved - run config.reload to apply)", argv[1], value.c_str());
  out(msg);
}

// Save the in-memory config to flash
static void cmd_config_save(int argc, char** argv, ShellOutput out) {
  // Config is already saved by config.set. This is for manual save after
  // programmatic changes to Config::get() that bypass config.set.
  JsonObject cfg = Config::get();
  File f = LittleFS.open("/config.json", "w");
  if (!f) { out("Failed to write config.json"); return; }
  size_t bytes = serializeJsonPretty(cfg, f);
  f.close();
  char msg[64];
  snprintf(msg, sizeof(msg), "Config saved to /config.json (%d bytes)", (int)bytes);
  out(msg);
}

// Reload config from flash. If network-affecting keys changed, reinit MQTT
// subscriptions and reconnect (reload-or-restart, like systemd).
static void cmd_config_reload(int argc, char** argv, ShellOutput out) {
  JsonObject oldCfg = Config::get();
  char oldPrefix[64], oldBroker[64], oldUser[64], oldPass[64], oldOtaTopic[96];
  uint16_t oldPort;
  strncpy(oldPrefix,   oldCfg["mqtt"]["topic_prefix"] | "thesada/node", sizeof(oldPrefix));
  strncpy(oldBroker,   oldCfg["mqtt"]["broker"]       | "",             sizeof(oldBroker));
  strncpy(oldUser,     oldCfg["mqtt"]["user"]          | "",             sizeof(oldUser));
  strncpy(oldPass,     oldCfg["mqtt"]["password"]      | "",             sizeof(oldPass));
  strncpy(oldOtaTopic, oldCfg["ota"]["cmd_topic"]      | "",             sizeof(oldOtaTopic));
  oldPort = oldCfg["mqtt"]["port"] | 8883;

  Config::load();
  JsonObject cfg = Config::get();
  const char* name = cfg["device"]["name"] | "?";

  bool networkChanged =
    strcmp(oldPrefix,   cfg["mqtt"]["topic_prefix"] | "thesada/node") != 0 ||
    strcmp(oldBroker,   cfg["mqtt"]["broker"]       | "")             != 0 ||
    strcmp(oldUser,     cfg["mqtt"]["user"]          | "")             != 0 ||
    strcmp(oldPass,     cfg["mqtt"]["password"]      | "")             != 0 ||
    strcmp(oldOtaTopic, cfg["ota"]["cmd_topic"]      | "")             != 0 ||
    oldPort != (cfg["mqtt"]["port"] | 8883);

  if (networkChanged) {
    out("Network config changed - reinitializing MQTT subscriptions");
    MQTTClient::reinitSubscriptions();
  }

  char msg[96];
  snprintf(msg, sizeof(msg), "Config reloaded from /config.json (device: %s)", name);
  out(msg);
}

// Print the full config as pretty-printed JSON
static void cmd_config_dump(int argc, char** argv, ShellOutput out) {
  JsonObject cfg = Config::get();
  String json;
  serializeJsonPretty(cfg, json);
  out(json.c_str());
}

// ---------------------------------------------------------------------------
// Network commands
// ---------------------------------------------------------------------------

// Show WiFi connection details and network info
static void cmd_ifconfig(int argc, char** argv, ShellOutput out) {
  char line[128];

  snprintf(line, sizeof(line), "WiFi: %s", WiFiManager::connected() ? "connected" : "disconnected");
  out(line);

  if (WiFiManager::connected()) {
    snprintf(line, sizeof(line), "  SSID: %s", WiFi.SSID().c_str());
    out(line);
    snprintf(line, sizeof(line), "  IP:   %s", WiFi.localIP().toString().c_str());
    out(line);
    snprintf(line, sizeof(line), "  GW:   %s", WiFi.gatewayIP().toString().c_str());
    out(line);
    snprintf(line, sizeof(line), "  DNS:  %s", WiFi.dnsIP().toString().c_str());
    out(line);
    snprintf(line, sizeof(line), "  RSSI: %d dBm", WiFi.RSSI());
    out(line);
    snprintf(line, sizeof(line), "  MAC:  %s", WiFi.macAddress().c_str());
    out(line);
  }
}

// Test connectivity by resolving a hostname via DNS
static void cmd_ping(int argc, char** argv, ShellOutput out) {
  if (argc < 2) { out("Usage: ping <host>"); return; }

  // ESP32 Arduino doesn't have a real ping. Use DNS resolve as a connectivity test.
  IPAddress ip;
  if (WiFi.hostByName(argv[1], ip)) {
    char msg[64];
    snprintf(msg, sizeof(msg), "%s resolved to %s", argv[1], ip.toString().c_str());
    out(msg);
  } else {
    char msg[64];
    snprintf(msg, sizeof(msg), "Failed to resolve %s", argv[1]);
    out(msg);
  }
}

// Show NTP status or manually set system time
static void cmd_ntp(int argc, char** argv, ShellOutput out) {
  char line[128];

  // ntp set <epoch> or ntp set <ISO8601>
  if (argc >= 3 && strcmp(argv[1], "set") == 0) {
    time_t epoch = 0;
    // Try epoch first (all digits).
    bool isEpoch = true;
    for (const char* p = argv[2]; *p; p++) {
      if (*p < '0' || *p > '9') { isEpoch = false; break; }
    }
    if (isEpoch) {
      epoch = (time_t)atol(argv[2]);
    } else {
      // Try ISO 8601: 2026-03-24T12:00:00Z
      struct tm parsed = {};
      if (strptime(argv[2], "%Y-%m-%dT%H:%M:%SZ", &parsed)) {
        epoch = mktime(&parsed);
      }
    }
    if (epoch > 1700000000UL) {
      struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
      settimeofday(&tv, nullptr);
      struct tm* t = gmtime(&epoch);
      char ts[32];
      strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", t);
      snprintf(line, sizeof(line), "Time set to %s (epoch %ld)", ts, (long)epoch);
      out(line);
    } else {
      out("Invalid time. Usage: ntp set <epoch> or ntp set 2026-03-24T12:00:00Z");
    }
    return;
  }

  time_t now = time(nullptr);
  struct tm* t = gmtime(&now);

  if (now < 1700000000) {
    out("NTP: not synced (time is before 2023)");
  } else {
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", t);
    snprintf(line, sizeof(line), "NTP: synced  UTC: %s  epoch: %ld", ts, (long)now);
    out(line);
  }

  JsonObject cfg = Config::get();
  const char* server = cfg["ntp"]["server"] | "pool.ntp.org";
  int offset = cfg["ntp"]["tz_offset_s"] | 0;
  snprintf(line, sizeof(line), "  server: %s  offset: %ds", server, offset);
  out(line);
  out(now > 1700000000UL ? "  log timestamps: active" : "  log timestamps: pending sync");
}

// Show MQTT connection status and broker info
// Print MQTT connection state, broker, prefix, and the full subscription table
// (slot index, topic, active flag). Used to diagnose subscription dispatch
// issues - e.g. a cmd_topic that silently stopped firing after reconnect.
static void cmd_mqtt(int argc, char** argv, ShellOutput out) {
  char line[160];
  snprintf(line, sizeof(line), "MQTT: %s", MQTTClient::connected() ? "connected" : "disconnected");
  out(line);

  JsonObject cfg = Config::get();
  const char* broker = cfg["mqtt"]["broker"] | "";
  int port = cfg["mqtt"]["port"] | 8883;
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "";
  snprintf(line, sizeof(line), "  broker: %s:%d  prefix: %s", broker, port, prefix);
  out(line);

  snprintf(line, sizeof(line), "  subs: %u/%u", (unsigned)MQTTClient::_subCount, (unsigned)MQTT_MAX_SUBS);
  out(line);
  for (uint8_t i = 0; i < MQTTClient::_subCount; i++) {
    snprintf(line, sizeof(line), "  [%u] %s %s",
             (unsigned)i,
             MQTTClient::_subs[i].topic,
             MQTTClient::_subs[i].active ? "active" : "inactive");
    out(line);
  }

  // RX ring - last N topics received at onMessage. If a topic was published
  // and broker delivered it, it shows here regardless of whether any
  // subscription callback matched.
  snprintf(line, sizeof(line), "  rx ring: %u/%u", (unsigned)MQTTClient::_rxRingCount, (unsigned)MQTTClient::RX_RING_SIZE);
  out(line);
  uint8_t count = MQTTClient::_rxRingCount;
  uint8_t head  = MQTTClient::_rxRingHead;
  uint32_t now  = millis();
  // Walk from oldest to newest
  for (uint8_t i = 0; i < count; i++) {
    uint8_t idx = (head + MQTTClient::RX_RING_SIZE - count + i) % MQTTClient::RX_RING_SIZE;
    uint32_t ageMs = now - MQTTClient::_rxRingTs[idx];
    snprintf(line, sizeof(line), "    %us ago: %s", (unsigned)(ageMs / 1000), MQTTClient::_rxRing[idx]);
    out(line);
  }
}

// ---------------------------------------------------------------------------
// OTA + chip diagnostic commands
// ---------------------------------------------------------------------------

// Trigger an OTA check immediately. Optional args:
//   ota.check              - fetch config manifest_url, install if newer
//   ota.check <url>        - fetch <url> manifest, install if newer
//   ota.check --force      - fetch config manifest_url, install regardless
//   ota.check --force <url>- fetch <url>, install regardless of version
// Force mode bypasses the isNewer() check so dev iteration doesn't need
// a FIRMWARE_VERSION bump every cycle.
static void cmd_ota_check(int argc, char** argv, ShellOutput out) {
  bool force = false;
  const char* url = nullptr;
  for (int i = 1; i < argc; i++) {
    if (argv[i] && strcmp(argv[i], "--force") == 0) {
      force = true;
    } else if (argv[i] && strlen(argv[i]) > 0 && !url) {
      url = argv[i];
    }
  }

  char line[192];
  snprintf(line, sizeof(line), "ota.check queued (force=%s, url=%s)",
           force ? "yes" : "no",
           url ? url : "config");
  out(line);
  out("check will run on next OTAUpdate::loop() tick - device may reboot shortly");
  // Non-blocking trigger. If we called OTAUpdate::check() directly here the
  // ESP.restart() at the end of a successful flash would fire before Shell
  // returned, so the cli/response would never publish.
  OTAUpdate::triggerCheck(url, force);
}

// Print running + boot + next-update partition info and OTA state. Uses the
// ESP-IDF esp_ota API directly so it works regardless of whether the Arduino
// Update wrapper has been touched this boot. Useful for diagnosing rollback
// state and confirming a freshly-flashed partition is marked valid.
static void cmd_ota_status(int argc, char** argv, ShellOutput out) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot    = esp_ota_get_boot_partition();
  const esp_partition_t* next    = esp_ota_get_next_update_partition(NULL);

  char line[160];
  if (running) {
    snprintf(line, sizeof(line), "running: %s  addr=0x%lx  size=%lu",
             running->label, (unsigned long)running->address, (unsigned long)running->size);
    out(line);
  } else {
    out("running: <unknown>");
  }
  if (boot) {
    snprintf(line, sizeof(line), "boot:    %s  addr=0x%lx",
             boot->label, (unsigned long)boot->address);
    out(line);
  }
  if (next) {
    snprintf(line, sizeof(line), "next:    %s  addr=0x%lx  size=%lu",
             next->label, (unsigned long)next->address, (unsigned long)next->size);
    out(line);
  }

  if (running) {
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
      const char* stateStr = "unknown";
      switch (state) {
        case ESP_OTA_IMG_NEW:            stateStr = "new"; break;
        case ESP_OTA_IMG_PENDING_VERIFY: stateStr = "pending_verify"; break;
        case ESP_OTA_IMG_VALID:          stateStr = "valid"; break;
        case ESP_OTA_IMG_INVALID:        stateStr = "invalid"; break;
        case ESP_OTA_IMG_ABORTED:        stateStr = "aborted"; break;
        case ESP_OTA_IMG_UNDEFINED:      stateStr = "undefined"; break;
      }
      snprintf(line, sizeof(line), "running state: %s", stateStr);
      out(line);
    }
  }

  snprintf(line, sizeof(line), "fw version: %s  built: %s %s",
           FIRMWARE_VERSION, __DATE__, __TIME__);
  out(line);
}

// Boot count (from RTC-memory survival across deep sleep via SleepManager)
// plus the last reset reason as a human-readable string. Cheap, no flash
// reads, safe to call in any state.
static void cmd_boot_info(int argc, char** argv, ShellOutput out) {
  esp_reset_reason_t reason = esp_reset_reason();
  const char* reasonStr = "unknown";
  switch (reason) {
    case ESP_RST_POWERON:  reasonStr = "power_on"; break;
    case ESP_RST_EXT:      reasonStr = "external_pin"; break;
    case ESP_RST_SW:       reasonStr = "sw_reset (ESP.restart())"; break;
    case ESP_RST_PANIC:    reasonStr = "panic / exception"; break;
    case ESP_RST_INT_WDT:  reasonStr = "interrupt_watchdog"; break;
    case ESP_RST_TASK_WDT: reasonStr = "task_watchdog"; break;
    case ESP_RST_WDT:      reasonStr = "other_watchdog"; break;
    case ESP_RST_DEEPSLEEP:reasonStr = "deep_sleep_wake"; break;
    case ESP_RST_BROWNOUT: reasonStr = "brownout"; break;
    case ESP_RST_SDIO:     reasonStr = "sdio_reset"; break;
    default: break;
  }
  char line[160];
  snprintf(line, sizeof(line), "reset reason: %s (%d)", reasonStr, (int)reason);
  out(line);

  snprintf(line, sizeof(line), "uptime: %lu ms", (unsigned long)millis());
  out(line);

  snprintf(line, sizeof(line), "fw version: %s  built: %s %s",
           FIRMWARE_VERSION, __DATE__, __TIME__);
  out(line);
}

// List all partitions (app + data) with label, type, subtype, offset, size.
// Uses esp_partition_next iterator - no hardcoded layout assumptions.
static void cmd_partitions(int argc, char** argv, ShellOutput out) {
  char line[160];
  out("partition table:");
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
  while (it) {
    const esp_partition_t* p = esp_partition_get(it);
    if (p) {
      snprintf(line, sizeof(line),
               "  %-10s type=%u sub=%u  addr=0x%08lx  size=%lu",
               p->label,
               (unsigned)p->type,
               (unsigned)p->subtype,
               (unsigned long)p->address,
               (unsigned long)p->size);
      out(line);
    }
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
}

// Dump selected CONFIG_* values from sdkconfig.h that affect OTA + partition
// + rollback behavior. Used to confirm the device's runtime config matches
// what the firmware was built against. No hidden state - all macros are
// evaluated at compile time.
static void cmd_sdkconfig(int argc, char** argv, ShellOutput out) {
  char line[160];

#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  out("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE: 1 (pending_verify auto-rollback on)");
#else
  out("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE: 0 (no auto-rollback)");
#endif
#ifdef CONFIG_APP_ROLLBACK_ENABLE
  out("CONFIG_APP_ROLLBACK_ENABLE: 1");
#else
  out("CONFIG_APP_ROLLBACK_ENABLE: 0");
#endif

#ifdef CONFIG_SPIRAM
  out("CONFIG_SPIRAM: 1 (PSRAM compiled in)");
#else
  out("CONFIG_SPIRAM: 0 (no PSRAM)");
#endif
#ifdef CONFIG_SPIRAM_MODE_OCT
  out("CONFIG_SPIRAM_MODE_OCT: 1 (octal PSRAM)");
#elif defined(CONFIG_SPIRAM_MODE_QUAD)
  out("CONFIG_SPIRAM_MODE_QUAD: 1 (quad PSRAM)");
#endif
#ifdef CONFIG_ESPTOOLPY_FLASHMODE_QIO
  out("CONFIG_ESPTOOLPY_FLASHMODE_QIO: 1");
#elif defined(CONFIG_ESPTOOLPY_FLASHMODE_DIO)
  out("CONFIG_ESPTOOLPY_FLASHMODE_DIO: 1");
#endif

#ifdef CONFIG_ESPTOOLPY_FLASHSIZE
  snprintf(line, sizeof(line), "CONFIG_ESPTOOLPY_FLASHSIZE: %s", CONFIG_ESPTOOLPY_FLASHSIZE);
  out(line);
#endif

#ifdef CONFIG_FREERTOS_HZ
  snprintf(line, sizeof(line), "CONFIG_FREERTOS_HZ: %d", CONFIG_FREERTOS_HZ);
  out(line);
#endif
#ifdef CONFIG_ESP_TASK_WDT_TIMEOUT_S
  snprintf(line, sizeof(line), "CONFIG_ESP_TASK_WDT_TIMEOUT_S: %d", CONFIG_ESP_TASK_WDT_TIMEOUT_S);
  out(line);
#endif
#ifdef CONFIG_ESP_INT_WDT_TIMEOUT_MS
  snprintf(line, sizeof(line), "CONFIG_ESP_INT_WDT_TIMEOUT_MS: %d", CONFIG_ESP_INT_WDT_TIMEOUT_MS);
  out(line);
#endif

  out("runtime esp_task_wdt_init: 30s (from main.cpp)");

#ifdef CONFIG_PARTITION_TABLE_FILENAME
  snprintf(line, sizeof(line), "CONFIG_PARTITION_TABLE_FILENAME: %s", CONFIG_PARTITION_TABLE_FILENAME);
  out(line);
#endif
}

// Chip revision, cores, flash size, PSRAM size. No hidden state - pure
// hardware facts, read from esp_chip_info + esp_flash + heap_caps.
static void cmd_chip_info(int argc, char** argv, ShellOutput out) {
  esp_chip_info_t info;
  esp_chip_info(&info);
  char line[160];

  const char* model = "?";
  switch (info.model) {
    case CHIP_ESP32:   model = "ESP32"; break;
    case CHIP_ESP32S2: model = "ESP32-S2"; break;
    case CHIP_ESP32S3: model = "ESP32-S3"; break;
    case CHIP_ESP32C3: model = "ESP32-C3"; break;
    default: break;
  }
  snprintf(line, sizeof(line), "chip: %s  rev=%u  cores=%u",
           model, (unsigned)info.revision, (unsigned)info.cores);
  out(line);

  uint32_t flashSize = 0;
  esp_flash_get_size(NULL, &flashSize);
  snprintf(line, sizeof(line), "flash: %lu bytes (%.2f MB)",
           (unsigned long)flashSize, flashSize / (1024.0 * 1024.0));
  out(line);

#if defined(BOARD_HAS_PSRAM)
  size_t psramSize = ESP.getPsramSize();
  size_t psramFree = ESP.getFreePsram();
  snprintf(line, sizeof(line), "psram: %u bytes free / %u bytes total",
           (unsigned)psramFree, (unsigned)psramSize);
  out(line);
#else
  out("psram: not compiled in (no BOARD_HAS_PSRAM)");
#endif

  snprintf(line, sizeof(line), "cpu freq: %lu MHz", (unsigned long)getCpuFrequencyMhz());
  out(line);
}

// ---------------------------------------------------------------------------
// Module commands
// ---------------------------------------------------------------------------

// List all modules and their compile-time enabled state
static void cmd_module_list(int argc, char** argv, ShellOutput out) {
  out("Compiled modules:");

  #ifdef ENABLE_TEMPERATURE
  out("  [x] temperature");
  #else
  out("  [ ] temperature");
  #endif

  #ifdef ENABLE_ADS1115
  out("  [x] ads1115");
  #else
  out("  [ ] ads1115");
  #endif

  #ifdef ENABLE_PWM
  out("  [x] pwm");
  #else
  out("  [ ] pwm");
  #endif

  #ifdef ENABLE_SD
  out("  [x] sd");
  #else
  out("  [ ] sd");
  #endif

  #ifdef ENABLE_CELLULAR
  out("  [x] cellular");
  #else
  out("  [ ] cellular");
  #endif

  #ifdef ENABLE_TELEGRAM
  out("  [x] telegram");
  #else
  out("  [ ] telegram");
  #endif

  #ifdef ENABLE_LORA
  out("  [x] lora");
  #else
  out("  [ ] lora");
  #endif

  #ifdef ENABLE_DISPLAY
  out("  [x] display");
  #else
  out("  [ ] display");
  #endif

  #ifdef ENABLE_TFT
  out("  [x] tftdisplay");
  #else
  out("  [ ] tftdisplay");
  #endif
}

// Show runtime status of each registered module
static void cmd_module_status(int argc, char** argv, ShellOutput out) {
  for (uint8_t i = 0; i < ModuleRegistry::count(); i++) {
    Module* m = ModuleRegistry::get(i);
    if (!m) continue;
    // Module name + status on one line
    char buf[160] = {};
    size_t off = snprintf(buf, sizeof(buf), "  %-14s ", m->name());
    m->status([&buf, &off](const char* s) {
      snprintf(buf + off, sizeof(buf) - off, "%s", s);
    });
    out(buf);
  }
}

// ---------------------------------------------------------------------------
// System commands
// ---------------------------------------------------------------------------

// Reboot the device
static void cmd_restart(int argc, char** argv, ShellOutput out) {
  out("Restarting...");
  delay(100);
  ESP.restart();
}

// Show free heap, minimum watermark, and largest allocatable block.
// Also shows PSRAM stats if PSRAM is present and enabled.
static void cmd_heap(int argc, char** argv, ShellOutput out) {
  char line[128];
  snprintf(line, sizeof(line), "Free: %lu B  Min: %lu B  Max alloc: %lu B",
           (unsigned long)ESP.getFreeHeap(),
           (unsigned long)ESP.getMinFreeHeap(),
           (unsigned long)ESP.getMaxAllocHeap());
  out(line);
#if defined(BOARD_HAS_PSRAM)
  if (psramFound()) {
    snprintf(line, sizeof(line),
             "PSRAM: free %lu B  size %lu B  min %lu B  max alloc %lu B",
             (unsigned long)ESP.getFreePsram(),
             (unsigned long)ESP.getPsramSize(),
             (unsigned long)ESP.getMinFreePsram(),
             (unsigned long)ESP.getMaxAllocPsram());
    out(line);
  } else {
    out("PSRAM: not detected (psramFound() == false)");
  }
#else
  out("PSRAM: not enabled in build");
#endif
}

// Show device uptime in days, hours, minutes, seconds
static void cmd_uptime(int argc, char** argv, ShellOutput out) {
  unsigned long s = millis() / 1000;
  char line[48];
  snprintf(line, sizeof(line), "%lud %02lu:%02lu:%02lu",
           s/86400, (s%86400)/3600, (s%3600)/60, s%60);
  out(line);
}

// Show firmware version and build timestamp
static void cmd_version(int argc, char** argv, ShellOutput out) {
  char line[96];
  snprintf(line, sizeof(line), "thesada-fw v%s (%s %s)", FIRMWARE_VERSION, __DATE__, __TIME__);
  out(line);
}

// Show all available shell commands
static void cmd_help(int argc, char** argv, ShellOutput out) {
  out("thesada-fw shell");
  out("");
  Shell::listCommands(out);
}

// List all configured sensors and their current readings
static void cmd_sensors(int argc, char** argv, ShellOutput out) {
  char line[128];
  int count = 0;

  // Temperature
  #ifdef ENABLE_TEMPERATURE
  {
    JsonObject cfg = Config::get();
    JsonArray sensors = cfg["temperature"]["sensors"].as<JsonArray>();
    if (!sensors.isNull()) {
      for (JsonObject s : sensors) {
        const char* name = s["name"] | "?";
        const char* addr = s["address"] | "";
        snprintf(line, sizeof(line), "  temp  %-12s %s", name, addr);
        out(line);
        count++;
      }
    }
  }
  #endif

  // ADS1115
  #ifdef ENABLE_ADS1115
  {
    JsonObject cfg = Config::get();
    JsonArray channels = cfg["ads1115"]["channels"].as<JsonArray>();
    if (!channels.isNull()) {
      for (JsonObject ch : channels) {
        const char* name = ch["name"] | "?";
        const char* mux  = ch["mux"]  | "?";
        float gain = ch["gain"] | 0.0f;
        snprintf(line, sizeof(line), "  adc   %-12s mux=%s gain=%.3f", name, mux, gain);
        out(line);
        count++;
      }
    }
  }
  #endif

  snprintf(line, sizeof(line), "%d sensor(s) configured", count);
  out(line);
}

// ---------------------------------------------------------------------------
// Self-test command
// ---------------------------------------------------------------------------

// Run diagnostic checks on all core subsystems
static void cmd_selftest(int argc, char** argv, ShellOutput out) {
  out("=== thesada-fw self-test ===");
  out("");
  char line[128];
  int pass = 0, fail = 0;

  // 1. Config
  JsonObject cfg = Config::get();
  const char* devName = cfg["device"]["name"] | "";
  if (strlen(devName) > 0) {
    snprintf(line, sizeof(line), "[PASS] Config loaded, device: %s", devName);
    out(line); pass++;
  } else {
    out("[FAIL] Config: device name empty or missing"); fail++;
  }

  // 2. WiFi
  if (WiFiManager::connected()) {
    snprintf(line, sizeof(line), "[PASS] WiFi connected, IP: %s, RSSI: %d dBm",
             WiFi.localIP().toString().c_str(), WiFi.RSSI());
    out(line); pass++;
  } else {
    out("[FAIL] WiFi not connected"); fail++;
  }

  // 3. NTP
  time_t now = time(nullptr);
  if (now > 1700000000) {
    out("[PASS] NTP synced"); pass++;
  } else {
    out("[FAIL] NTP not synced (time before 2023)"); fail++;
  }

  // 4. MQTT
  if (MQTTClient::connected()) {
    out("[PASS] MQTT connected"); pass++;
  } else {
    out("[FAIL] MQTT not connected"); fail++;
  }

  // 5. LittleFS
  if (LittleFS.exists("/config.json")) {
    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();
    snprintf(line, sizeof(line), "[PASS] LittleFS mounted, %d/%d bytes used", (int)used, (int)total);
    out(line); pass++;
  } else {
    out("[FAIL] LittleFS: /config.json not found"); fail++;
  }

  // 6. SD card - reported via Module::selftest() in SDModule

  // 7. CA cert
  if (LittleFS.exists("/ca.crt")) {
    File cf = LittleFS.open("/ca.crt", "r");
    if (cf && cf.size() > 100) {
      snprintf(line, sizeof(line), "[PASS] /ca.crt present (%d bytes)", (int)cf.size());
      out(line); pass++;
      cf.close();
    } else {
      out("[WARN] /ca.crt exists but seems too small");
      if (cf) cf.close();
    }
  } else {
    out("[WARN] /ca.crt not found (MQTT TLS using insecure mode)");
  }

  // 8. Module self-tests
  for (uint8_t i = 0; i < ModuleRegistry::count(); i++) {
    Module* m = ModuleRegistry::get(i);
    if (!m) continue;
    m->selftest([&out, &pass, &fail](const char* line) {
      out(line);
      if (strncmp(line, "[PASS]", 6) == 0) pass++;
      else if (strncmp(line, "[FAIL]", 6) == 0) fail++;
    });
  }

  // 9. Scripts
  if (LittleFS.exists("/scripts/main.lua")) {
    out("[PASS] /scripts/main.lua present"); pass++;
  } else {
    out("[WARN] /scripts/main.lua not found");
  }

  if (LittleFS.exists("/scripts/rules.lua")) {
    out("[PASS] /scripts/rules.lua present"); pass++;
  } else {
    out("[INFO] /scripts/rules.lua not found (optional)");
  }

  // Summary
  out("");
  snprintf(line, sizeof(line), "=== %d passed, %d failed ===", pass, fail);
  out(line);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

// Register all built-in shell commands
void Shell::registerBuiltins() {
  // System
  registerCommand("help",          "Show all commands",             cmd_help);
  registerCommand("version",       "Firmware version and build",    cmd_version);
  registerCommand("restart",       "Reboot device",                 cmd_restart);
  registerCommand("heap",          "Free heap memory",              cmd_heap);
  registerCommand("uptime",        "Device uptime",                 cmd_uptime);
  registerCommand("sensors",       "Sensor readings info",          cmd_sensors);
  registerCommand("selftest",      "Run self-test checks",          cmd_selftest);

  // Filesystem
  registerCommand("fs.ls",         "List directory (fs.ls [path])", cmd_ls);
  registerCommand("fs.cat",        "Print file contents",           cmd_cat);
  registerCommand("fs.rm",         "Remove a file",                 cmd_rm);
  registerCommand("fs.write",      "Write content to file",         cmd_write);
  registerCommand("fs.mv",         "Rename/move a file",            cmd_mv);
  registerCommand("fs.df",         "Disk usage (LittleFS + SD)",    cmd_df);

  // Config
  registerCommand("config.get",    "Read config key (dot notation)",   cmd_config_get);
  registerCommand("config.set",    "Set config key (saves + reloads)", cmd_config_set);
  registerCommand("config.save",   "Save current config to flash",     cmd_config_save);
  registerCommand("config.reload", "Reload config from flash",         cmd_config_reload);
  registerCommand("config.dump",   "Print full config JSON",           cmd_config_dump);

  // Network
  registerCommand("net.ip",        "Network interface info",        cmd_ifconfig);
  registerCommand("net.ping",      "DNS resolve test",              cmd_ping);
  registerCommand("net.ntp",       "NTP status / net.ntp set <epoch|ISO8601>", cmd_ntp);
  registerCommand("net.mqtt",      "MQTT connection + subscription dump", cmd_mqtt);
  registerCommand("ota.check",     "Trigger OTA check (ota.check [--force] [url])", cmd_ota_check);
  registerCommand("ota.status",    "Partition + rollback state",    cmd_ota_status);
  registerCommand("boot.info",     "Last reset reason + uptime",    cmd_boot_info);
  registerCommand("partitions",    "Full partition table dump",     cmd_partitions);
  registerCommand("chip.info",     "Chip revision, flash, PSRAM",   cmd_chip_info);
  registerCommand("sdkconfig",     "Selected CONFIG_* relevant to OTA/boot", cmd_sdkconfig);

  // Module
  registerCommand("module.list",   "List compiled modules",         cmd_module_list);
  registerCommand("module.status", "Module status overview",        cmd_module_status);
}

// Initialize the shell and register built-in commands
void Shell::begin() {
  registerBuiltins();

  char msg[48];
  snprintf(msg, sizeof(msg), "Shell ready - %d commands", _commandCount);
  Log::info("Shell", msg);
}
