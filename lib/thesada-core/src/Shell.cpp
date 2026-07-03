// thesada-fw - Shell.cpp
// SPDX-License-Identifier: GPL-3.0-only

#include "Shell.h"
#include "Config.h"
#include "Secret.h"
#include "Console.h"
#include "Log.h"
#include "WiFiManager.h"
#include "MQTTClient.h"
#include "ModuleRegistry.h"
#include "OTAUpdate.h"
#include "SensorRegistry.h"
#include "Net.h"
#include <thesada_config.h>
#include <mbedtls/platform_util.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_task_wdt.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_system.h>
#include <esp_app_format.h>

#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <sys/time.h>
#include <algorithm>

ShellEntry Shell::_commands[MAX_COMMANDS];
int Shell::_commandCount = 0;
char Shell::_parseBuf[256];

Shell::FSMount Shell::_fsMounts[Shell::FS_MOUNTS_MAX] = {};
int            Shell::_fsMountCount = 0;

Shell::DeferredSlot Shell::_ring[Shell::DEFERRED_RING_SIZE] = {};
uint8_t             Shell::_ringHead = 0;
uint8_t             Shell::_ringTail = 0;

// portMUX guards _ring + indices against concurrent enqueue from non-main
// task contexts (AsyncTCP onEvent, PubSubClient callback). The critical
// sections are short - just memcpy + index bump - so spinning in the other
// task is fine.
static portMUX_TYPE _shellRingMux = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

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

void Shell::registerCommand(const char* name, const char* help, ShellCommand handler) {
  if (_commandCount >= MAX_COMMANDS) return;
  _commands[_commandCount] = {name, help, handler};
  _commandCount++;
}

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

// Stage a command for main-loop execution via the deferred dispatcher.
// Capture by-value of the sink intentional - the caller's lambda may
// reference task-local state that goes out of scope before loop()
// fires; std::function's small-buffer optimization keeps trivial
// captures inline.
bool Shell::enqueue(const char* line, ShellOutput sink) {
  if (!line || strlen(line) == 0) return false;
  bool accepted = false;
  taskENTER_CRITICAL(&_shellRingMux);
  uint8_t next = (uint8_t)((_ringTail + 1) % DEFERRED_RING_SIZE);
  if (next != _ringHead) {  // not full
    DeferredSlot& slot = _ring[_ringTail];
    slot.mode = SlotMode::Shell;
    strncpy(slot.line, line, sizeof(slot.line) - 1);
    slot.line[sizeof(slot.line) - 1] = '\0';
    slot.sink = sink;
    slot.fn = nullptr;
    slot.active = true;
    _ringTail = next;
    accepted = true;
  }
  taskEXIT_CRITICAL(&_shellRingMux);
  return accepted;
}

// Stage an arbitrary callable. Same ring + same backpressure as enqueue();
// drain dispatches by mode. Captures live inside std::function (heap on
// non-trivial captures) - acceptable given the ring's 4-slot ceiling.
bool Shell::enqueueDeferred(DeferredFn fn) {
  if (!fn) return false;
  bool accepted = false;
  taskENTER_CRITICAL(&_shellRingMux);
  uint8_t next = (uint8_t)((_ringTail + 1) % DEFERRED_RING_SIZE);
  if (next != _ringHead) {
    DeferredSlot& slot = _ring[_ringTail];
    slot.mode = SlotMode::Handler;
    slot.line[0] = '\0';
    slot.sink = nullptr;
    slot.fn = std::move(fn);
    slot.active = true;
    _ringTail = next;
    accepted = true;
  }
  taskEXIT_CRITICAL(&_shellRingMux);
  return accepted;
}

// Drain at most one staged command per tick. Wired into the main loop so
// every enqueued command runs with full main-loop stack and not from
// inside an async/network callback. Capture line + sink under the mutex,
// then run execute() outside the critical section so a long-running
// command (fs reads, LittleFS writes, MQTT publishes) does not block
// concurrent enqueuers.
//
// Move (not copy) the std::function payloads into locals while inside the
// portMUX critical section. Previous behaviour copied sink and then nulled
// slot.sink/slot.fn under the lock - the assignment destroyed the captured
// state (heap allocations, std::string captures) inside the spinlock,
// which on ESP-IDF can deadlock the allocator if it takes its internal
// lock. Moving leaves the slot's std::function in the empty/moved-from
// state (no destruction needed); destructors of the locals run at function
// return, outside the critical section.
void Shell::loop() {
  char line[DEFERRED_LINE_LEN];
  ShellOutput sink;
  DeferredFn  fn;
  SlotMode    mode = SlotMode::Empty;

  taskENTER_CRITICAL(&_shellRingMux);
  if (_ringHead != _ringTail && _ring[_ringHead].active) {
    DeferredSlot& slot = _ring[_ringHead];
    mode = slot.mode;
    if (mode == SlotMode::Shell) {
      memcpy(line, slot.line, sizeof(line));
      sink = std::move(slot.sink);
    } else if (mode == SlotMode::Handler) {
      fn = std::move(slot.fn);
    }
    slot.mode = SlotMode::Empty;
    slot.active = false;
    // Do NOT null slot.sink / slot.fn here - they are already in the
    // moved-from (empty) state. Nulling would run the destructor of the
    // previous payload inside the critical section.
    _ringHead = (uint8_t)((_ringHead + 1) % DEFERRED_RING_SIZE);
  }
  taskEXIT_CRITICAL(&_shellRingMux);

  if (mode == SlotMode::Shell) {
    execute(line, sink);
  } else if (mode == SlotMode::Handler && fn) {
    fn();
  }
}

// Single console - safe to share static buffer state because there is only
// one host typing at the other end.
void Shell::pumpConsole() {
  static char  buf[Shell::DEFERRED_LINE_LEN];
  static int   pos = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      buf[pos] = '\0';
      if (pos > 0) {
        Shell::execute(buf, [](const char* line) { Console::reply(line); });
        Console::endReply();
      }
      pos = 0;
    } else if (pos < (int)sizeof(buf) - 1) {
      buf[pos++] = c;
    }
  }
}

void Shell::listCommands(ShellOutput out) {
  for (int i = 0; i < _commandCount; i++) {
    char line[128];
    snprintf(line, sizeof(line), "  %-16s %s", _commands[i].name, _commands[i].help);
    out(line);
  }
}

// Categorised help. With nullptr / empty filter: enumerate dot-prefix
// categories (each shown once with command count) + emit no-dot
// commands inline so `restart`, `version`, etc remain discoverable.
// With filter "cell": print every command whose first dot-token equals
// "cell", showing full name + help text.
//
// Buckets are derived on the fly (no separate registry) - any newly
// registered cell.* / net.* / etc command shows up automatically.
void Shell::printHelp(const char* filter, ShellOutput out) {
  bool listing = (filter != nullptr && filter[0] != '\0');

  if (listing) {
    // Filter mode: dump every command whose first dot-token matches.
    size_t flen = strlen(filter);
    char header[64];
    snprintf(header, sizeof(header), "%s.* commands:", filter);
    out(header);
    int hits = 0;
    for (int i = 0; i < _commandCount; i++) {
      const char* name = _commands[i].name;
      const char* dot  = strchr(name, '.');
      // Match if name starts with filter AND either filter is the full
      // command (no-dot match) or filter ends right at the first '.'.
      bool match = false;
      if (dot) {
        if ((size_t)(dot - name) == flen && strncasecmp(name, filter, flen) == 0) match = true;
      } else if (strcasecmp(name, filter) == 0) {
        match = true;
      }
      if (!match) continue;
      char line[128];
      snprintf(line, sizeof(line), "  %-20s %s", name, _commands[i].help);
      out(line);
      hits++;
    }
    if (hits == 0) {
      out("  (no commands in that category - try `help` for the list)");
    }
    return;
  }

  // Bucket mode: dedupe categories, count members. Tracks at most
  // MAX_COMMANDS distinct prefixes which is more than we will ever
  // hit (one bucket per command at worst).
  out("thesada-fw shell - commands grouped by category");
  out("type `help <category>` to expand (e.g. `help cell`)");
  out("");
  out("Categories:");

  char    seen[MAX_COMMANDS][20];
  int     counts[MAX_COMMANDS] = {0};
  int     bucketCount = 0;

  for (int i = 0; i < _commandCount; i++) {
    const char* name = _commands[i].name;
    const char* dot  = strchr(name, '.');
    if (!dot) continue;
    size_t pfxLen = dot - name;
    if (pfxLen >= sizeof(seen[0])) pfxLen = sizeof(seen[0]) - 1;
    int found = -1;
    for (int j = 0; j < bucketCount; j++) {
      if (strncmp(seen[j], name, pfxLen) == 0 && seen[j][pfxLen] == '\0') {
        found = j; break;
      }
    }
    if (found < 0) {
      if (bucketCount >= MAX_COMMANDS) continue;
      memcpy(seen[bucketCount], name, pfxLen);
      seen[bucketCount][pfxLen] = '\0';
      counts[bucketCount] = 1;
      bucketCount++;
    } else {
      counts[found]++;
    }
  }
  for (int j = 0; j < bucketCount; j++) {
    char line[64];
    snprintf(line, sizeof(line), "  %-12s (%d cmd%s)",
             seen[j], counts[j], counts[j] == 1 ? "" : "s");
    out(line);
  }

  out("");
  out("Top-level:");
  for (int i = 0; i < _commandCount; i++) {
    if (strchr(_commands[i].name, '.')) continue;
    char line[128];
    snprintf(line, sizeof(line), "  %-20s %s", _commands[i].name, _commands[i].help);
    out(line);
  }
}

// ---------------------------------------------------------------------------
// Filesystem commands
// ---------------------------------------------------------------------------

static FS* resolveFS(const char* path) { return Shell::resolveFS(path); }
static const char* stripPrefix(const char* path) {
  return Shell::stripPrefix(path);
}

// fs.* commands dispatch to the right backing filesystem by matching the
// path's leading segment against the prefix registry. SDModule registers
// "/sd" in its begin() after a successful mount; everything else falls
// through to LittleFS. Same dispatch is used by the MQTT cli binary
// handlers (fs.write / fs.cat chunked) so all transports see the same
// routing.
fs::FS* Shell::resolveFS(const char* path) {
  if (!path) return &LittleFS;
  for (int i = 0; i < _fsMountCount; i++) {
    const FSMount& m = _fsMounts[i];
    // Match either the bare prefix (e.g. "/sd") or prefix-with-separator
    // (e.g. "/sd/log042.csv"). Anything else falls through.
    if (strncmp(path, m.prefix, m.prefixLen) == 0 &&
        (path[m.prefixLen] == '\0' || path[m.prefixLen] == '/')) {
      return m.fs;
    }
  }
  return &LittleFS;
}

const char* Shell::stripPrefix(const char* path) {
  if (!path) return path;
  for (int i = 0; i < _fsMountCount; i++) {
    const FSMount& m = _fsMounts[i];
    if (strncmp(path, m.prefix, m.prefixLen) == 0 &&
        (path[m.prefixLen] == '\0' || path[m.prefixLen] == '/')) {
      // Bare prefix or trailing slash -> root of underlying FS.
      const char* rest = path + m.prefixLen;
      if (*rest == '\0') return "/";
      if (rest[0] == '/' && rest[1] == '\0') return "/";
      return rest;
    }
  }
  return path;
}

bool Shell::registerFS(const char* prefix, fs::FS* fs) {
  return registerFS(prefix, fs, nullptr, nullptr);
}

bool Shell::registerFS(const char* prefix, fs::FS* fs, FSDfFn dfUsed, FSDfFn dfTotal) {
  if (!prefix || !fs) return false;
  if (prefix[0] != '/') return false;
  size_t len = strlen(prefix);
  if (len < 2 || prefix[len - 1] == '/') return false;
  if (_fsMountCount >= FS_MOUNTS_MAX) {
    Log::warn("Shell", "FS mount table full");
    return false;
  }
  _fsMounts[_fsMountCount++] = { prefix, len, fs, dfUsed, dfTotal };
  char msg[64];
  snprintf(msg, sizeof(msg), "FS mount: %s -> %p", prefix, (void*)fs);
  Log::info("Shell", msg);
  return true;
}

void Shell::listMounts(ShellOutput out) {
  char line[64];
  for (int i = 0; i < _fsMountCount; i++) {
    snprintf(line, sizeof(line), "  [MOUNT] %s", _fsMounts[i].prefix);
    out(line);
  }
}

// Member function so it can reach the private FSMount registry;
// cmd_df handles LittleFS inline then calls this.
void Shell::printRegisteredDf(ShellOutput out) {
  char line[128];
  for (int i = 0; i < _fsMountCount; i++) {
    const FSMount& m = _fsMounts[i];
    if (!m.dfUsed || !m.dfTotal) continue;
    uint64_t mtotal = m.dfTotal();
    uint64_t mused  = m.dfUsed();
    if (mtotal == 0) {
      snprintf(line, sizeof(line), "%s: not mounted", m.prefix);
    } else {
      snprintf(line, sizeof(line),
               "%s: %llu / %llu bytes used (%llu%% free)",
               m.prefix, (unsigned long long)mused, (unsigned long long)mtotal,
               (unsigned long long)((mtotal - mused) * 100 / mtotal));
    }
    out(line);
  }
}

// Centralised so every transport (Shell-over-serial/WS, HTTP /api/file,
// MQTT CLI binary handlers) shares one policy. See Shell::pathSafe in Shell.h.
// in:  null-terminated path. out: true if safe.
bool Shell::pathSafe(const char* path) {
  if (!path || !*path) return false;
  if (path[0] != '/') return false;
  if (strstr(path, "..") != nullptr) return false;
  if (strstr(path, "//") != nullptr) return false;
  return true;
}

static void cmd_ls(int argc, char** argv, ShellOutput out) {
  const char* path = (argc > 1) ? argv[1] : "/";
  if (!Shell::pathSafe(path)) { out("Invalid path"); return; }
  FS* fs = resolveFS(path);
  const char* fsPath = stripPrefix(path);

  File dir = fs->open(fsPath);
  if (!dir || !dir.isDirectory()) {
    out("Not a directory or not found");
    return;
  }

  // Build a base path that entries can be appended to with a single slash.
  // "/" stays as "/". "/scripts" or "/scripts/" both become "/scripts".
  // Paths go back out in absolute form so user can copy-paste into fs.cat.
  char base[96];
  strncpy(base, path, sizeof(base) - 1);
  base[sizeof(base) - 1] = '\0';
  size_t bl = strlen(base);
  while (bl > 1 && base[bl - 1] == '/') { base[--bl] = '\0'; }

  char line[160];
  File entry = dir.openNextFile();
  while (entry) {
    if (entry.isDirectory()) {
      snprintf(line, sizeof(line), "  [DIR]  %s/%s/", base, entry.name());
    } else {
      snprintf(line, sizeof(line), "  %6d  %s/%s",
               (int)entry.size(), base, entry.name());
    }
    // "//foo" collapses to "/foo" when base is "/".
    char* dbl = strstr(line, "//");
    while (dbl) { memmove(dbl, dbl + 1, strlen(dbl)); dbl = strstr(line, "//"); }
    out(line);
    entry = dir.openNextFile();
  }

  // Bare `fs.ls` lists the LittleFS root only. Append a discovery line per
  // registered mount prefix so an SD card (or future external volume) is
  // visible without the operator already knowing to type `fs.ls /sd`.
  // An explicit path argument suppresses this - the caller asked for a
  // specific directory, not the mount overview.
  if (argc <= 1) {
    Shell::listMounts(out);
  }
}

static void cmd_cat(int argc, char** argv, ShellOutput out) {
  if (argc < 2) { out("Usage: cat <path>"); return; }
  if (!Shell::pathSafe(argv[1])) { out("Invalid path"); return; }
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

static void cmd_rm(int argc, char** argv, ShellOutput out) {
  if (argc < 2) { out("Usage: rm <path>"); return; }
  if (!Shell::pathSafe(argv[1])) { out("Invalid path"); return; }
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

// Write content to a file. Registered for both fs.write (truncate) and
// fs.append (append) - the open mode is selected from argv[0] so the two
// commands share one handler. Mirrors the MQTT binary-handler path in
// MQTTClient::runCli, which already keys the mode off the command name.
static void cmd_write(int argc, char** argv, ShellOutput out) {
  // fs.write|fs.append <path> <content...>
  if (argc < 3) { out("Usage: write <path> <content...>"); return; }
  if (!Shell::pathSafe(argv[1])) { out("Invalid path"); return; }
  FS* fs = resolveFS(argv[1]);
  const char* fsPath = stripPrefix(argv[1]);

  bool append = (strcasecmp(argv[0], "fs.append") == 0);

  String content;
  for (int i = 2; i < argc; i++) {
    if (i > 2) content += " ";
    content += argv[i];
  }

  File f = fs->open(fsPath, append ? "a" : "w");
  if (!f) { out("Failed to open for writing"); return; }
  f.print(content);
  f.close();

  char msg[64];
  snprintf(msg, sizeof(msg), "%s %d bytes to %s",
           append ? "Appended" : "Wrote", content.length(), fsPath);
  out(msg);
}

// Rename or move a file
static void cmd_mv(int argc, char** argv, ShellOutput out) {
  if (argc < 3) { out("Usage: mv <src> <dst>"); return; }
  if (!Shell::pathSafe(argv[1]) || !Shell::pathSafe(argv[2])) {
    out("Invalid path"); return;
  }
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

// Show disk usage for LittleFS plus every registered filesystem that
// advertises df support (dfUsed/dfTotal pointers in its FSMount entry).
// LittleFS is handled inline - it is always present and core owns it.
// Other volumes (SD via SDModule) come from the mount registry so core
// stays one-way independent of the modules: cmd_df never calls SDModule.
//
// totalBytes() returns 0 when a volume is not mounted (fresh flash, mount
// failure, post-partition-table migration). The previous version divided
// by total to compute free-percent and panic'ed with IntegerDivideByZero
// in exactly that case - every branch here guards total == 0.
static void cmd_df(int argc, char** argv, ShellOutput out) {
  char line[128];

  size_t total = LittleFS.totalBytes();
  size_t used  = LittleFS.usedBytes();
  if (total == 0) {
    out("LittleFS: not mounted");
  } else {
    snprintf(line, sizeof(line), "LittleFS: %d / %d bytes used (%d%% free)",
             (int)used, (int)total, (int)((total - used) * 100 / total));
    out(line);
  }

  Shell::printRegisteredDf(out);
}

// Reformat LittleFS - destroys every file. Requires `fs.format --yes` so
// a stray `fs.format` press over MQTT cannot wipe a device. Use case:
// recover from a corrupt / zombie-dirent state when targeted fs.rm
// cannot reach the bad entry. Always reboots after format so the rest
// of the firmware (Lua state, MQTT subs, OTA timers) starts fresh
// against the empty filesystem instead of crashing on stale handles.
// Caller is responsible for re-uploading config.json + ca.crt + scripts.
//
// In:  argv[1] must be "--yes"
// Out: status line, then reboot
static void cmd_format(int argc, char** argv, ShellOutput out) {
  if (argc < 2 || strcmp(argv[1], "--yes") != 0) {
    out("Usage: fs.format --yes  (DESTROYS every file on LittleFS)");
    return;
  }
  LittleFS.end();
  bool ok = LittleFS.format();
  out(ok ? "LittleFS reformatted - rebooting" : "LittleFS format FAILED - rebooting anyway");
  delay(200);
  esp_restart();
}

// ---------------------------------------------------------------------------
// Config commands
// ---------------------------------------------------------------------------

// Parse `tok` as an unsigned integer for use as a JsonArray index.
// in:  tok    candidate token string
// out: bool   true if every char is a digit (and at least one digit), idx filled
static bool parseArrayIndex(const char* tok, size_t& idx) {
  if (!tok || !*tok) return false;
  size_t v = 0;
  for (const char* p = tok; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    v = v * 10 + (size_t)(*p - '0');
  }
  idx = v;
  return true;
}

// Read a config value by dot-notation key. Supports array indices
// (e.g. wifi.networks.0.ssid - "0" descends into the JsonArray returned
// by wifi.networks).
static void cmd_config_get(int argc, char** argv, ShellOutput out) {
  if (argc < 2) { out("Usage: config.get <key>  (dot notation, e.g. wifi.networks.0.ssid)"); return; }

  JsonObject cfg = Config::get();
  JsonVariant current = cfg;

  char key[128];
  strncpy(key, argv[1], sizeof(key) - 1);
  key[sizeof(key) - 1] = '\0';

  char* token = strtok(key, ".");
  while (token) {
    size_t idx;
    if (current.is<JsonArray>() && parseArrayIndex(token, idx)) {
      JsonArray arr = current.as<JsonArray>();
      if (idx >= arr.size()) { out("Array index out of range"); return; }
      current = arr[idx];
    } else if (current.is<JsonObject>()) {
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

static void cmd_config_set(int argc, char** argv, ShellOutput out) {
  if (argc < 3) { out("Usage: config.set <key> <value>  (then config.save to persist)"); return; }

  String value;
  for (int i = 2; i < argc; i++) {
    if (i > 2) value += " ";
    value += argv[i];
  }
  // Strip surrounding double quotes (MQTT CLI payloads often arrive quoted)
  if (value.length() >= 2 && value.charAt(0) == '"' && value.charAt(value.length() - 1) == '"') {
    value = value.substring(1, value.length() - 1);
  }

  if (!LittleFS.exists("/config.json")) { out("config.json not found"); return; }
  File f = LittleFS.open("/config.json", "r");
  if (!f) { out("Failed to read config.json"); return; }
  String json = f.readString();
  f.close();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) { out("JSON parse error in config.json"); return; }

  char key[128];
  strncpy(key, argv[1], sizeof(key) - 1);
  key[sizeof(key) - 1] = '\0';

  char* lastDot = strrchr(key, '.');
  if (lastDot) {
    *lastDot = '\0';
    const char* finalKey = lastDot + 1;
    String parentPath(key);  // parent path with dots intact, for error messages

    JsonVariant parent = doc.as<JsonVariant>();
    String walked;           // dotted path resolved so far
    char* saveptr = nullptr;
    char* token = strtok_r(key, ".", &saveptr);
    while (token) {
      // Peek the next segment so an auto-created intermediate knows
      // whether the user actually wants an array at this level.
      char* nextToken = strtok_r(nullptr, ".", &saveptr);

      size_t idx;
      if (parent.is<JsonArray>() && parseArrayIndex(token, idx)) {
        JsonArray arr = parent.as<JsonArray>();
        if (idx >= arr.size()) { out("Parent array index out of range"); return; }
        parent = arr[idx];
      } else if (parent.is<JsonObject>()) {
        // Auto-create missing intermediate objects so config.set can land
        // a value into a section that does not exist yet (e.g. enabling a
        // brand-new optional module like gnss). Never auto-creates arrays:
        // if the next segment is a numeric index the user wants an array
        // here, so refuse with a clear instruction rather than silently
        // creating an object the index cannot address.
        if (parent[token].isNull()) {
          size_t dummy;
          if (nextToken && parseArrayIndex(nextToken, dummy)) {
            String path = walked.length() ? walked + "." + token : String(token);
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "Parent path '%s' doesn't exist. Run config.set %s [] first.",
                     path.c_str(), path.c_str());
            out(msg);
            return;
          }
          parent[token].to<JsonObject>();
        }
        parent = parent[token];
      } else {
        out("Parent key not found");
        return;
      }
      walked = walked.length() ? walked + "." + token : String(token);
      token = nextToken;
    }

    // Resolve a writable target for the final key. JsonObject member
    // assignment + JsonArray index assignment use different ArduinoJson
    // APIs; capture the destination as a JsonVariant once so the value
    // detection below is target-agnostic.
    //
    // Append semantics on arrays: if the terminal index equals
    // arr.size() the slot is appended via arr.add<JsonVariant>() so
    // numeric-suffix config.set paths naturally extend an array. Strict
    // idx > size rejection still prevents a sparse array from ever
    // forming.
    // A numeric final segment only addresses an array. If the parent
    // resolved to an object the array does not exist yet - refuse rather
    // than creating a string-keyed member that merely looks like an index.
    JsonVariant target;
    size_t finalIdx;
    if (parseArrayIndex(finalKey, finalIdx) && !parent.is<JsonArray>()) {
      char msg[160];
      snprintf(msg, sizeof(msg),
               "Parent path '%s' is not an array. Run config.set %s [] first.",
               parentPath.c_str(), parentPath.c_str());
      out(msg);
      return;
    }
    if (parent.is<JsonArray>() && parseArrayIndex(finalKey, finalIdx)) {
      JsonArray arr = parent.as<JsonArray>();
      if (finalIdx > arr.size()) {
        out("Array index out of range (would leave hole)");
        return;
      }
      if (finalIdx == arr.size()) {
        target = arr.add<JsonVariant>();
      } else {
        target = arr[finalIdx];
      }
    } else if (parent.is<JsonObject>()) {
      // ArduinoJson's operator[] yields an *unbound* variant for a key
      // that does not exist yet; capturing that into `target` and
      // calling target.set() later writes into nothing. That is why
      // config.set could update an existing key but silently failed to
      // create a new one. Materialise the member first (mirrors the
      // intermediate-object auto-create above) so the captured handle
      // binds to a real document slot. The placeholder object is
      // overwritten by the value assignment below.
      if (parent[finalKey].isNull()) {
        parent[finalKey].to<JsonObject>();
      }
      target = parent[finalKey];
    } else {
      out("Parent is not an object or array");
      return;
    }

    if (value == "--delete") {
      if (parent.is<JsonObject>()) {
        parent[finalKey].clear();
        parent.as<JsonObject>().remove(finalKey);
      } else {
        // For arrays we set the slot to null; remove() would shift
        // indices and silently break sibling references.
        target.clear();
      }
      goto save;
    }

    // Assign as a Variant so config.set wifi.networks [{"ssid":"..."}] stores
    // a real JsonArray, not a stringified blob (prior fallthrough behaviour).
    {
      String trimmed = value;
      trimmed.trim();
      if (trimmed.length() > 0 &&
          (trimmed.charAt(0) == '{' || trimmed.charAt(0) == '[')) {
        JsonDocument lit;
        DeserializationError lerr = deserializeJson(lit, trimmed);
        if (!lerr) {
          target.set(lit.as<JsonVariant>());
          goto save;
        }
        // Parse failed - fall through to scalar detection. The user
        // probably typed a string that just happens to start with `{`.
      }
    }

    if (value == "true") target.set(true);
    else if (value == "false") target.set(false);
    else {
      char* end;
      long lv = strtol(value.c_str(), &end, 10);
      if (*end == '\0' && value.length() > 0) {
        target.set(lv);
      } else {
        float fv = strtof(value.c_str(), &end);
        if (*end == '\0' && value.length() > 0) {
          target.set(fv);
        } else {
          target.set(value);
        }
      }
    }
  } else {
    // Top-level key.
    out("Cannot set top-level keys directly (use section.key format)");
    return;
  }

  save:
  File wf = LittleFS.open("/config.json", "w");
  if (!wf) { out("Failed to write config.json"); return; }
  serializeJsonPretty(doc, wf);
  wf.close();

  // Does NOT trigger MQTT reconnect - that requires config.reload.
  Config::load();

  char msg[128];
  if (value == "--delete") {
    snprintf(msg, sizeof(msg), "Deleted %s (saved)", argv[1]);
  } else {
    snprintf(msg, sizeof(msg), "Set %s = %s (saved - run config.reload to apply network changes)", argv[1], value.c_str());
  }
  out(msg);
}

// For programmatic changes to Config::get() that bypass config.set.
static void cmd_config_save(int argc, char** argv, ShellOutput out) {
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

  // Republish the retained set (status, HA discovery, /info, retained-topics
  // manifest) so any subscriber polling those topics sees fresh state
  // without waiting for a reboot or MQTT session drop. /info carries the
  // config_hash that downstream consumers diff to decide whether to pull
  // the new config. HA discovery refresh is also useful when sensor names
  // or units moved in the config. Force=true bypasses the once-per-session
  // guard.
  MQTTClient::publishRetainedSet(true);

  char msg[96];
  snprintf(msg, sizeof(msg), "Config reloaded from /config.json (device: %s)", name);
  out(msg);
}

static void cmd_config_dump(int argc, char** argv, ShellOutput out) {
  JsonObject cfg = Config::get();
  String json;
  serializeJsonPretty(cfg, json);
  out(json.c_str());
}

// ---------------------------------------------------------------------------
// Network commands
// ---------------------------------------------------------------------------

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

  // Cellular transport - shown only when the cellular module is compiled
  // in. Reported as a separate stack from WiFi; no claim is made about
  // which one the OS routes through when both are up.
  const Net::CellularProvider* cell = Net::cellular();
  if (cell && cell->linkUp && cell->linkUp()) {
    out("Cellular: connected");
    if (cell->linkInfo) {
      cell->linkInfo([&](const char* info) {
        char l[160];
        snprintf(l, sizeof(l), "  %s", info);
        out(l);
      });
    }
  } else if (cell) {
    out("Cellular: down");
  }
}

// Test connectivity by resolving a hostname via DNS. Routes by transport:
// WiFi up -> lwIP DNS; WiFi down + cellular up -> modem DNS (AT+CDNSGIP),
// because with WiFi down lwIP has no default route and WiFi.hostByName
// would never reach a resolver.
static void cmd_ping(int argc, char** argv, ShellOutput out) {
  if (argc < 2) { out("Usage: ping <host>"); return; }

  // ESP32 Arduino doesn't have a real ping. Use DNS resolve as a connectivity test.
  if (WiFiManager::connected()) {
    IPAddress ip;
    if (WiFi.hostByName(argv[1], ip)) {
      char msg[96];
      snprintf(msg, sizeof(msg), "%s resolved to %s (via WiFi)",
               argv[1], ip.toString().c_str());
      out(msg);
    } else {
      char msg[96];
      snprintf(msg, sizeof(msg), "Failed to resolve %s (via WiFi)", argv[1]);
      out(msg);
    }
    return;
  }

  const Net::CellularProvider* cell = Net::cellular();
  if (cell && cell->linkUp && cell->linkUp() && cell->resolve) {
    char ip[48];
    if (cell->resolve(argv[1], ip, sizeof(ip))) {
      char msg[96];
      snprintf(msg, sizeof(msg), "%s resolved to %s (via cellular)", argv[1], ip);
      out(msg);
    } else {
      char msg[96];
      snprintf(msg, sizeof(msg), "Failed to resolve %s (via cellular)", argv[1]);
      out(msg);
    }
    return;
  }

  out("No transport up - cannot resolve");
}

static void cmd_ntp(int argc, char** argv, ShellOutput out) {
  char line[128];

  if (argc >= 3 && strcmp(argv[1], "set") == 0) {
    time_t epoch = 0;
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

  // ntp sync - force a sync now. Over WiFi the SNTP client runs in the
  // background already; over cellular nothing syncs the clock unless the
  // modem is asked (AT+CNTP), so this is the recovery path for the
  // "WiFi never associated, clock stuck at 1970" case.
  if (argc >= 2 && strcmp(argv[1], "sync") == 0) {
    JsonObject  scfg   = Config::get();
    const char* server = scfg["ntp"]["server"] | "pool.ntp.org";
    if (WiFiManager::connected()) {
      out("WiFi up - SNTP client already syncing in background");
      return;
    }
    const Net::CellularProvider* cell = Net::cellular();
    if (cell && cell->linkUp && cell->linkUp() && cell->ntpSync) {
      uint32_t timeoutMs = (scfg["ntp"]["cell_timeout_s"] | 60) * 1000UL;
      out("Syncing clock via cellular NTP...");
      if (cell->ntpSync(server, timeoutMs)) {
        time_t now = time(nullptr);
        struct tm* t = gmtime(&now);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", t);
        snprintf(line, sizeof(line), "Synced via cellular - UTC: %s", ts);
        out(line);
      } else {
        out("Cellular NTP sync failed");
      }
      return;
    }
    out("No transport up - cannot sync");
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

// Prints full subscription table (slot, topic, active) to diagnose
// dispatch issues - e.g. a cmd_topic that silently stopped firing after reconnect.
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

  // Surface which transport carries the session - cellular takes over
  // when WiFi is down (see CellularModule's STANDBY/ACTIVE policy), so a
  // debug session sees the carrier without grepping serial logs.
  const Net::CellularProvider* cell = Net::cellular();
  bool viaCellular = cell && cell->linkUp && cell->linkUp() && !WiFiManager::connected();
  snprintf(line, sizeof(line), "  transport: %s", viaCellular ? "cellular" : "WiFi");
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

// net.http body-preview cap: the first N bytes of the response body are
// echoed back to the shell; the rest is counted but discarded.
static const size_t HTTP_PREVIEW_CAP = 1024;
static const size_t HTTP_URL_MAX     = 256;

// One net.http WiFi fetch. Heap-allocated by cmd_http and handed to
// httpFetchTask; cmd_http owns it and frees it once `done` is observed.
struct HttpFetchJob {
  // in
  char          url[HTTP_URL_MAX];
  bool          https;
  bool          insecure;
  // out
  char          preview[HTTP_PREVIEW_CAP];
  size_t        previewLen;
  size_t        totalLen;
  int           status;
  bool          ok;
  const char*   err;        // static message, nullptr on success
  volatile bool done;       // task sets this last; cmd_http polls it
};

// Run the WiFi HTTPS fetch on a dedicated deep-stack task.
//
// Why a task: the mbedtls TLS handshake needs ~6 KB of stack, and
// net.http executes deep in the deferred-CLI call chain on the main-loop
// task (loop -> Shell::loop -> deferred drain -> execute -> cmd_http).
// Doing the handshake there overflows the 8 KB loop-task stack - the
// board resets silently (panic out the other UART, USB-CDC dead). This
// task gets its own 12 KB stack; cmd_http blocks on job->done.
//
// The task is not subscribed to the task watchdog, so it does not feed
// it - the main-loop task keeps feeding its own wdt from cmd_http's wait
// loop. Every exit path sets job->done and self-deletes.
// in:  arg - HttpFetchJob* (owned by cmd_http). out: job fields filled.
static void httpFetchTask(void* arg) {
  HttpFetchJob* job = static_cast<HttpFetchJob*>(arg);

  WiFiClientSecure tls;
  WiFiClient       plain;
  tls.setHandshakeTimeout(10);  // seconds; the 120 s default is past any sane bound

  if (job->https) {
    // Reuse the same CA the OTA path uses - no second copy in code.
    File ca = LittleFS.open("/ca.crt", "r");
    if (ca) {
      String pem = ca.readString();
      ca.close();
      tls.setCACert(pem.c_str());
    } else if (job->insecure) {
      tls.setInsecure();
    } else {
      job->err  = "no /ca.crt on flash - pass --insecure to skip verification";
      job->done = true;
      vTaskDelete(nullptr);
      return;
    }
  }

  HTTPClient http;
  bool begun = job->https ? http.begin(tls, job->url)
                          : http.begin(plain, job->url);
  if (!begun) {
    job->err  = "http.begin failed";
    job->done = true;
    vTaskDelete(nullptr);
    return;
  }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setConnectTimeout(10000);
  http.setTimeout(10000);

  job->status = http.GET();
  if (job->status > 0) {
    job->ok = true;
    int         len      = http.getSize();  // -1 when chunked / unknown
    WiFiClient* s        = http.getStreamPtr();
    uint8_t     chunk[512];
    uint32_t    deadline = millis() + 20000UL;
    while (millis() < deadline) {
      if (!s->connected() && !s->available()) break;
      int avail = s->available();
      if (avail <= 0) { delay(10); continue; }
      int n = s->readBytes(chunk, std::min((size_t)avail, sizeof(chunk)));
      if (n <= 0) break;
      job->totalLen += n;
      if (job->previewLen < HTTP_PREVIEW_CAP) {
        size_t copy = std::min((size_t)n, HTTP_PREVIEW_CAP - job->previewLen);
        memcpy(job->preview + job->previewLen, chunk, copy);
        job->previewLen += copy;
      }
      if (len > 0 && (int)job->totalLen >= len) break;
    }
  }
  http.end();
  job->done = true;
  vTaskDelete(nullptr);
}

// HTTPS GET via whichever transport is up - the single HTTPS-GET shell
// command, auto-routed. WiFi -> a deep-stack task running HTTPClient +
// WiFiClientSecure (CA loaded from /ca.crt). WiFi down + cellular up ->
// the modem SSL socket via the Net cellular provider (no ESP-side
// mbedtls, so it runs inline). Output: status code, body length, first
// <=1 KB preview. HTTPS-only by default; plain HTTP needs --insecure.
//
// in:  argv - optional "--insecure", then the URL
// out: status / length / preview lines via the shell sink
static void cmd_http(int argc, char** argv, ShellOutput out) {
  if (argc < 2) { out("usage: net.http [--insecure] <url>"); return; }

  bool        insecure = false;
  const char* url      = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--insecure") == 0) insecure = true;
    else                                    url = argv[i];
  }
  if (!url) { out("usage: net.http [--insecure] <url>"); return; }
  if (strlen(url) >= HTTP_URL_MAX) { out("url too long"); return; }

  const char* p     = url;
  uint16_t    port  = 443;
  bool        https = true;
  if (strncmp(p, "https://", 8) == 0)     { p += 8; port = 443; https = true; }
  else if (strncmp(p, "http://", 7) == 0) { p += 7; port = 80;  https = false; }
  else { out("url must start with http:// or https://"); return; }

  if (!https && !insecure) {
    out("plain HTTP refused - pass --insecure to allow");
    return;
  }

  char        host[128];
  const char* slash   = strchr(p, '/');
  const char* hostEnd = slash ? slash : p + strlen(p);
  size_t      hostLen = hostEnd - p;
  if (hostLen == 0 || hostLen >= sizeof(host)) { out("bad host"); return; }
  memcpy(host, p, hostLen);
  host[hostLen] = '\0';
  char* colon = strchr(host, ':');
  if (colon) { *colon = '\0'; port = atoi(colon + 1); }
  const char* path = slash ? slash : "/";

  char msg[224];
  snprintf(msg, sizeof(msg), "GET %s://%s:%u%s",
           https ? "https" : "http", host, port, path);
  out(msg);

  HttpFetchJob* job = static_cast<HttpFetchJob*>(calloc(1, sizeof(HttpFetchJob)));
  if (!job) { out("net.http: out of memory"); return; }
  strncpy(job->url, url, sizeof(job->url) - 1);
  job->https    = https;
  job->insecure = insecure;

  const char* via = "?";

  if (WiFiManager::connected()) {
    via = "WiFi";
    // The WiFi HTTPS path runs on a dedicated deep-stack task (see
    // httpFetchTask); cmd_http blocks here feeding the main-task
    // watchdog and pumping the console while it waits.
    if (xTaskCreate(httpFetchTask, "net.http", 12288, job,
                    tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
      out("net.http: failed to spawn fetch task");
      free(job);
      return;
    }
    // The task's own bounds are ~8 s connect + 8 s handshake + 20 s read;
    // 60 s is a comfortable ceiling. job is freed only after done is
    // observed, so the task cannot write into freed memory on this path.
    uint32_t deadline = millis() + 60000UL;
    while (!job->done && millis() < deadline) {
      esp_task_wdt_reset();
      Shell::pumpConsole();
      delay(20);
    }
    if (!job->done) {
      // Unreachable given the task's internal bounds. Leak the job rather
      // than free it - the task may still hold the pointer.
      out("net.http: fetch task overran - leaking job buffer");
      return;
    }
  } else {
    const Net::CellularProvider* cell = Net::cellular();
    if (cell && cell->linkUp && cell->linkUp() && cell->httpsGet) {
      if (!https) { out("cellular path is HTTPS only"); free(job); return; }
      via = "cellular";
      // No ESP-side mbedtls on the cellular path (the modem does TLS), so
      // this runs inline on the main-loop task without the stack hit.
      auto sink = [&](const uint8_t* buf, size_t len) -> bool {
        job->totalLen += len;
        if (job->previewLen < HTTP_PREVIEW_CAP) {
          size_t copy = std::min(len, HTTP_PREVIEW_CAP - job->previewLen);
          memcpy(job->preview + job->previewLen, buf, copy);
          job->previewLen += copy;
        }
        return true;
      };
      job->ok = cell->httpsGet(host, path, port, sink, &job->status);
    } else {
      out("no transport up");
      free(job);
      return;
    }
  }

  if (job->err) {
    out(job->err);
    free(job);
    return;
  }

  snprintf(msg, sizeof(msg),
           "status=%d ok=%d body=%u bytes (preview <=%u, via %s)",
           job->status, (int)job->ok, (unsigned)job->totalLen,
           (unsigned)job->previewLen, via);
  out(msg);

  size_t start = 0;
  for (size_t i = 0; i < job->previewLen; ++i) {
    if (job->preview[i] == '\n' || job->preview[i] == '\r') {
      if (i > start) {
        char l[256];
        size_t n = std::min(i - start, sizeof(l) - 1);
        memcpy(l, job->preview + start, n);
        l[n] = '\0';
        out(l);
      }
      start = i + 1;
    }
  }
  if (start < job->previewLen) {
    char l[256];
    size_t n = std::min(job->previewLen - start, sizeof(l) - 1);
    memcpy(l, job->preview + start, n);
    l[n] = '\0';
    out(l);
  }
  out("--- done ---");
  free(job);
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

// Print stored mTLS client cert metadata (CN, serial, validity). Never
// exposes the private key - info-only. No args.
// in: argc, argv, out. out: prints cert info lines, or "no cert stored"
static void cmd_cert_info(int argc, char** argv, ShellOutput out) {
  // Check raw NVS existence first so a stored-but-unparseable cert
  // reports "corrupt" instead of "not stored".
  Preferences prefs;
  bool hasCertBlob = false, hasKeyBlob = false;
  if (prefs.begin("thesada-tls", true)) {
    // Stored as strings via putString - use isKey alone (getBytesLength
    // is for blob type and logs an error for string keys).
    hasCertBlob = prefs.isKey("client_cert");
    hasKeyBlob  = prefs.isKey("client_key");
    prefs.end();
  }

  if (!hasCertBlob && !hasKeyBlob) {
    out("No client cert stored (using password auth)");
    return;
  }

  char cn[128], serial[96], notBefore[32], notAfter[32];
  bool parsed = hasCertBlob && MQTTClient::getCertInfo(cn, serial, notBefore, notAfter, sizeof(cn));

  if (parsed) {
    char line[192];
    snprintf(line, sizeof(line), "CN:         %s", cn);         out(line);
    snprintf(line, sizeof(line), "Serial:     %s", serial);     out(line);
    snprintf(line, sizeof(line), "Not before: %s", notBefore);  out(line);
    snprintf(line, sizeof(line), "Not after:  %s", notAfter);   out(line);
  } else if (hasCertBlob) {
    out("Cert stored but unparseable (corrupt or not valid PEM)");
  }

  if (hasCertBlob && hasKeyBlob) {
    out("Status:     cert + key in NVS - cert.apply to reconnect with mTLS");
  } else if (hasCertBlob) {
    out("Status:     cert stored, KEY MISSING - push client_key via cli/cert.set");
  } else {
    out("Status:     key stored, CERT MISSING - push client_cert via cli/cert.set");
  }
}

// Apply a newly-pushed client cert by disconnecting MQTT. The loop() path
// then reconnects and (if both cert + key are now in NVS) sets up mTLS.
// in: argc, argv, out. out: status line
static void cmd_cert_apply(int argc, char** argv, ShellOutput out) {
  if (!MQTTClient::hasClientCert()) {
    out("No cert + key in NVS - push both via cli/cert.set before apply");
    return;
  }
  // Disconnect alone is not enough on classic-platform boards: WiFiClientSecure
  // holds sticky mbedtls state across the cert swap and the next handshake
  // silently drops the freshly-loaded cert. Reboot is the only
  // reliable recovery. Schedule it 3 s out so this handler can publish its
  // cli/response first and the main loop gets one tick to flush the socket.
  out("Cert + key present - rebooting in 3s to apply (mTLS on next boot)");
  MQTTClient::_certApplyRebootAtMs    = millis() + 3000;
  MQTTClient::_certApplyRebootPending = true;
}

// Erase stored mTLS cert + key from NVS and force a reconnect. Device
// falls back to password auth on next connect. Safe if no cert present.
// in: argc, argv, out. out: status line
static void cmd_cert_clear(int argc, char** argv, ShellOutput out) {
  bool had = MQTTClient::hasClientCert();
  if (!MQTTClient::clearClientCert()) {
    out("NVS clear failed");
    return;
  }
  if (had) {
    out("Client cert + key cleared - reconnecting with password auth");
    MQTTClient::_client.disconnect();
    MQTTClient::_wifiClient.stop();
  } else {
    out("No cert to clear (already using password auth)");
  }
}

// Provision a per-device secret into NVS. Fields: mqtt.password,
// telegram.bot_token, web.password, wifi.ap_password, wifi.password:<ssid>.
static void cmd_secret_set(int argc, char** argv, ShellOutput out) {
  if (argc < 3) { out("Usage: secret.set <field> <value>"); return; }
  String value;
  for (int i = 2; i < argc; i++) { if (i > 2) value += " "; value += argv[i]; }
  if (value.length() >= 2 && value.charAt(0) == '"' &&
      value.charAt(value.length() - 1) == '"') {
    value = value.substring(1, value.length() - 1);
  }
  bool ok = Secret::set(argv[1], value.c_str());
  // Wipe the plaintext secret from the String's heap buffer before it frees.
  mbedtls_platform_zeroize((void*)value.c_str(), value.length());
  out(ok ? "secret stored in NVS" : "unknown field or NVS write failed");
}

// Erase a secret from NVS; the read site falls back to config.json/empty.
static void cmd_secret_clear(int argc, char** argv, ShellOutput out) {
  if (argc < 2) { out("Usage: secret.clear <field>"); return; }
  out(Secret::clear(argv[1]) ? "secret cleared (falls back to config.json/empty)"
                             : "unknown field");
}

// Presence report only - never prints a secret value (write-only contract).
static void cmd_secret_info(int argc, char** argv, ShellOutput out) {
  const char* scalars[] = { "mqtt.password", "telegram.bot_token",
                            "web.password", "wifi.ap_password" };
  char line[128];
  for (auto f : scalars) {
    snprintf(line, sizeof(line), "%-22s %s", f, Secret::has(f) ? "nvs" : "config/none");
    out(line);
  }
  JsonArray nets = Config::get()["wifi"]["networks"].as<JsonArray>();
  for (JsonObject net : nets) {
    const char* ssid = net["ssid"] | "";
    if (!*ssid) continue;
    char field[80];
    snprintf(field, sizeof(field), "wifi.password:%s", ssid);
    snprintf(line, sizeof(line), "%-22s %s", field, Secret::has(field) ? "nvs" : "config/none");
    out(line);
  }
}

// Uses the ESP-IDF esp_ota API directly so it works regardless of whether
// the Arduino Update wrapper has been touched this boot.
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

// No flash reads; safe to call in any state.
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

// All macros evaluated at compile time - confirms runtime build config
// matches what the firmware was compiled against.
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

// Both module commands walk ModuleRegistry - a module compiled into the build
// is exactly one that self-registered via MODULE_REGISTER, so list and status
// always show the same set (the old hardcoded #ifdef list here drifted and
// omitted registered modules like HttpServer/ScriptEngine/GNSS).
static void cmd_module_list(int argc, char** argv, ShellOutput out) {
  out("Registered modules ([x] enabled, [ ] disabled by config):");
  for (uint8_t i = 0; i < ModuleRegistry::count(); i++) {
    Module* m = ModuleRegistry::get(i);
    if (!m) continue;
    char line[96];
    snprintf(line, sizeof(line), "  [%c] %-14s %s%s",
             ModuleRegistry::enabled(i) ? 'x' : ' ',
             m->configKey(), m->name(), m->coreModule() ? " (core)" : "");
    out(line);
  }
}

static void cmd_module_status(int argc, char** argv, ShellOutput out) {
  for (uint8_t i = 0; i < ModuleRegistry::count(); i++) {
    Module* m = ModuleRegistry::get(i);
    if (!m) continue;
    // Disabled modules never ran begin(); don't call status() on uninit state.
    if (!ModuleRegistry::enabled(i)) {
      char line[64];
      snprintf(line, sizeof(line), "  %-14s disabled", m->name());
      out(line);
      continue;
    }
    // Emit every status() line; continuation lines align under the first
    // (the old single-buffer version overwrote, so only the last line of a
    // multi-line status survived).
    bool first = true;
    m->status([&out, &first, m](const char* s) {
      char line[176];
      if (first) {
        snprintf(line, sizeof(line), "  %-14s %s", m->name(), s);
        first = false;
      } else {
        snprintf(line, sizeof(line), "  %-14s %s", "", s);
      }
      out(line);
    });
    if (first) {  // status() emitted nothing
      char line[64];
      snprintf(line, sizeof(line), "  %-14s (no status)", m->name());
      out(line);
    }
  }
}

// ---------------------------------------------------------------------------
// System commands
// ---------------------------------------------------------------------------

static void cmd_restart(int argc, char** argv, ShellOutput out) {
  out("Restarting...");
  delay(100);
  ESP.restart();
}

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

static void cmd_uptime(int argc, char** argv, ShellOutput out) {
  unsigned long s = millis() / 1000;
  char line[48];
  snprintf(line, sizeof(line), "%lud %02lu:%02lu:%02lu",
           s/86400, (s%86400)/3600, (s%3600)/60, s%60);
  out(line);
}

// Device name prints on a second line so scripts that grep `thesada-fw v...`
// keep working unchanged.
static void cmd_version(int argc, char** argv, ShellOutput out) {
  char line[96];
  snprintf(line, sizeof(line), "thesada-fw v%s (%s %s)", FIRMWARE_VERSION, __DATE__, __TIME__);
  out(line);

  JsonObject cfg = Config::get();
  const char* devName   = cfg["device"]["name"]        | "";
  const char* topicPref = cfg["mqtt"]["topic_prefix"]  | "";
  snprintf(line, sizeof(line), "device: %s  topic: %s",
           devName[0]   ? devName   : "?",
           topicPref[0] ? topicPref : "?");
  out(line);
}

// Collapses dotted commands into category buckets to avoid the multi-screen
// wall the flat list became as command count grew.
static void cmd_help(int argc, char** argv, ShellOutput out) {
  Shell::printHelp(argc > 1 ? argv[1] : nullptr, out);
}

// Modules self-register a read callback in begin(); dispatches by name
// or iterates all. `sensors all` runs every registered sensor in turn.
static void cmd_sensors(int argc, char** argv, ShellOutput out) {
  if (argc <= 1) {
    char line[96];
    snprintf(line, sizeof(line), "%d sensor(s) registered:", SensorRegistry::count());
    out(line);
    SensorRegistry::forEach([](const SensorEntry& e, ShellOutput o) {
      char l[128];
      snprintf(l, sizeof(l), "  %-12s %s%s",
               e.name,
               e.desc ? e.desc : "",
               e.enabled ? "" : "  (disabled)");
      o(l);
    }, out);
    out("usage: sensors <name> | sensors all");
    return;
  }

  if (strcmp(argv[1], "all") == 0) {
    SensorRegistry::forEach([](const SensorEntry& e, ShellOutput o) {
      char hdr[64];
      snprintf(hdr, sizeof(hdr), "[%s]", e.name);
      o(hdr);
      if (!e.enabled) { o("  (disabled)"); return; }
      e.read(o, e.ctx);
    }, out);
    return;
  }

  const SensorEntry* e = SensorRegistry::find(argv[1]);
  if (!e) {
    char line[96];
    snprintf(line, sizeof(line), "unknown sensor: %s", argv[1]);
    out(line);
    return;
  }
  if (!e->enabled) {
    out("sensor disabled in config");
    return;
  }
  e->read(out, e->ctx);
}

// ---------------------------------------------------------------------------
// Self-test command
// ---------------------------------------------------------------------------

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

  // 8. Module self-tests. Skip disabled modules - begin() never ran.
  for (uint8_t i = 0; i < ModuleRegistry::count(); i++) {
    Module* m = ModuleRegistry::get(i);
    if (!m || !ModuleRegistry::enabled(i)) continue;
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

// Get/set the serial console mode. Resets to normal on reboot.
static void cmd_console_mode(int argc, char** argv, ShellOutput out) {
  if (argc >= 2) {
    if (strcmp(argv[1], "command") == 0) {
      Console::setMode(Console::Mode::Command);
    } else if (strcmp(argv[1], "normal") == 0) {
      Console::setMode(Console::Mode::Normal);
    } else {
      out("Usage: console.mode [normal|command]");
      return;
    }
  }
  out(Console::mode() == Console::Mode::Command ? "mode: command" : "mode: normal");
}

// Register all built-in shell commands
void Shell::registerBuiltins() {
  // System
  registerCommand("help",          "Show categories or expand one (help <cat>)", cmd_help);
  registerCommand("version",       "Firmware version and build",    cmd_version);
  registerCommand("console.mode",  "Get/set console mode (normal|command)", cmd_console_mode);
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
  registerCommand("fs.append",     "Append content to file",        cmd_write);
  registerCommand("fs.mv",         "Rename/move a file",            cmd_mv);
  registerCommand("fs.df",         "Disk usage (LittleFS + SD)",    cmd_df);
  registerCommand("fs.format",     "Reformat LittleFS (fs.format --yes)", cmd_format);

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
  registerCommand("net.http",      "HTTPS GET via active transport (net.http [--insecure] <url>)", cmd_http);
  registerCommand("ota.check",     "Trigger OTA check (ota.check [--force] [url])", cmd_ota_check);
  registerCommand("ota.status",    "Partition + rollback state",    cmd_ota_status);

  // mTLS client cert management (NVS-backed)
  registerCommand("cert.info",     "Show stored client cert metadata",  cmd_cert_info);
  registerCommand("cert.apply",    "Reconnect MQTT with stored cert",   cmd_cert_apply);
  registerCommand("cert.clear",    "Erase client cert from NVS",        cmd_cert_clear);
  registerCommand("secret.set",    "Provision a secret into NVS",       cmd_secret_set);
  registerCommand("secret.info",   "Show which secrets are NVS-backed", cmd_secret_info);
  registerCommand("secret.clear",  "Erase a secret from NVS",           cmd_secret_clear);
  registerCommand("boot.info",     "Last reset reason + uptime",    cmd_boot_info);
  registerCommand("partitions",    "Full partition table dump",     cmd_partitions);
  registerCommand("chip.info",     "Chip revision, flash, PSRAM",   cmd_chip_info);
  registerCommand("sdkconfig",     "Selected CONFIG_* relevant to OTA/boot", cmd_sdkconfig);

  // Module
  registerCommand("module.list",   "List compiled modules",         cmd_module_list);
  registerCommand("module.status", "Module status overview",        cmd_module_status);
}

void Shell::begin() {
  registerBuiltins();

  char msg[48];
  snprintf(msg, sizeof(msg), "Shell ready - %d commands", _commandCount);
  Log::info("Shell", msg);
}
