// thesada-fw - SDModule.cpp
// SD card logger with dual mode support: SD_MMC (LILYGO) or SPI (CYD).
// SPDX-License-Identifier: GPL-3.0-only
#include <thesada_config.h>
#include "SDModule.h"
#include <Config.h>
#include <EventBus.h>
#include <Log.h>
#include <Shell.h>
#include <SD_MMC.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <time.h>
#include <ModuleRegistry.h>

static const char* TAG = "SD";

bool     SDModule::_mounted   = false;
bool     SDModule::_spiMode   = false;
String   SDModule::_logPath   = "";
uint32_t SDModule::_logBytes  = 0;
uint32_t SDModule::_maxBytes  = 0;

// ---------------------------------------------------------------------------

// Return the active filesystem (SD or SD_MMC depending on mode)
fs::FS* SDModule::fs() {
  if (_spiMode) return &SD;
  return &SD_MMC;
}

// fs.df probes for the SD volume. The fs::FS base class has no byte-count
// API - totalBytes()/usedBytes() live on the SD / SD_MMC subclasses - so
// these wrappers pick the right concrete object by _spiMode and get
// registered with the FS mount so Shell::cmd_df can report SD without
// core depending on this module. Returns 0 when no card is mounted.
uint64_t SDModule::_dfTotal() {
  return _spiMode ? SD.totalBytes() : SD_MMC.totalBytes();
}
uint64_t SDModule::_dfUsed() {
  return _spiMode ? SD.usedBytes() : SD_MMC.usedBytes();
}

// Find the next free log slot (log001-log999) and create a CSV file with header
bool SDModule::_openNextLog() {
  static const char* HEADER = "timestamp,sensor,data\n";
  fs::FS* sdfs = fs();
  for (int i = 1; i <= 999; i++) {
    char path[20];
    snprintf(path, sizeof(path), "/log%03d.csv", i);
    if (!sdfs->exists(path)) {
      File f = sdfs->open(path, FILE_WRITE);
      if (!f) return false;
      f.print(HEADER);
      f.close();
      _logPath  = path;
      _logBytes = strlen(HEADER);
      return true;
    }
  }
  return false;
}

// Pick the active log file on boot.
//
// Walk log999 -> log001 looking for the highest-numbered existing file.
// If found and under _maxBytes, resume appending to it: _logPath = path,
// _logBytes = on-disk size. Avoids the per-boot empty-file pile-up
// (Forgejo #312). If the highest file is at/over the limit, or none
// exist, fall back to _openNextLog() to claim a fresh slot.
//
// Returns true on success (either resumed or new slot opened).
bool SDModule::_resumeOrOpen() {
  fs::FS* sdfs = fs();
  for (int i = 999; i >= 1; i--) {
    char path[20];
    snprintf(path, sizeof(path), "/log%03d.csv", i);
    if (!sdfs->exists(path)) continue;

    File f = sdfs->open(path, FILE_READ);
    if (!f) return _openNextLog();
    uint32_t sz = (uint32_t)f.size();
    f.close();

    if (_maxBytes == 0 || sz < _maxBytes) {
      _logPath  = path;
      _logBytes = sz;
      return true;
    }
    return _openNextLog();
  }
  return _openNextLog();
}

// Rotate to a new log file when the current one exceeds the size limit
void SDModule::_rotate() {
  char msg[48];
  // TODO: migrate to structured logging
  snprintf(msg, sizeof(msg), "Rotating - %s full", _logPath.c_str());
  Log::info(TAG, msg);
  if (!_openNextLog()) {
    Log::error(TAG, "No free log slot - stopping rotation");
    _maxBytes = 0;
    return;
  }
  // TODO: migrate to structured logging
  snprintf(msg, sizeof(msg), "Logging to %s", _logPath.c_str());
  Log::info(TAG, msg);
}

// Mount the SD card, open the first log file, and subscribe to sensor events
void SDModule::begin() {
  JsonObject cfg = Config::get();

  const char* mode = cfg["sd"]["mode"] | "sdmmc";
  _spiMode = (strcmp(mode, "spi") == 0);

  uint32_t maxKb = cfg["sd"]["max_file_kb"] | 1024;
  _maxBytes = maxKb > 0 ? maxKb * 1024 : 0;

  if (_spiMode) {
    // SPI mode - CYD and other boards with SD on the shared SPI bus.
    // Pass the existing SPI instance so we don't re-init the bus
    // (TFT may have already claimed it).
    uint8_t cs = cfg["sd"]["pin_cs"] | 5;
    // Deselect SD CS before init to avoid bus contention with TFT
    pinMode(cs, OUTPUT);
    digitalWrite(cs, HIGH);
    delay(10);
    if (!SD.begin(cs, SPI, 4000000)) {
      Log::error(TAG, "SPI mount failed - check wiring or card");
      return;
    }
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
      Log::error(TAG, "No card detected");
      SD.end();
      return;
    }
    _mounted = true;
    char info[80];
    // TODO: migrate to structured logging
    snprintf(info, sizeof(info), "Mounted (SPI, CS=%d) - %.1f MB",
             cs, (float)SD.totalBytes() / (1024.0f * 1024.0f));
    Log::info(TAG, info);
    Shell::registerFS("/sd", &SD, _dfUsed, _dfTotal);
  } else {
    // SD_MMC mode - LILYGO and boards with dedicated SDMMC pins
    uint8_t pinClk  = cfg["sd"]["pin_clk"]  | 38;
    uint8_t pinCmd  = cfg["sd"]["pin_cmd"]  | 39;
    uint8_t pinData = cfg["sd"]["pin_data"] | 40;
    SD_MMC.setPins(pinClk, pinCmd, pinData);
    if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT)) {
      Log::error(TAG, "SD_MMC mount failed - check wiring or card");
      return;
    }
    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) {
      Log::error(TAG, "No card detected");
      SD_MMC.end();
      return;
    }
    _mounted = true;
    char info[80];
    // TODO: migrate to structured logging
    snprintf(info, sizeof(info), "Mounted (SD_MMC) - %.1f MB",
             (float)SD_MMC.totalBytes() / (1024.0f * 1024.0f));
    Log::info(TAG, info);
    Shell::registerFS("/sd", &SD_MMC, _dfUsed, _dfTotal);
  }

  if (!_resumeOrOpen()) {
    Log::error(TAG, "Could not find free log slot (log001-log999 all exist?)");
    return;
  }

  char info[96];
  if (_maxBytes > 0)
    // TODO: migrate to structured logging
    snprintf(info, sizeof(info), "Logging to %s  (%lu B, max %lu KB per file)",
             _logPath.c_str(), (unsigned long)_logBytes, (unsigned long)maxKb);
  else
    // TODO: migrate to structured logging
    snprintf(info, sizeof(info), "Logging to %s  (%lu B, no size limit)",
             _logPath.c_str(), (unsigned long)_logBytes);
  Log::info(TAG, info);

  subscribeEvents();
}

// ---------------------------------------------------------------------------

// No periodic work - logging is event-driven
void SDModule::loop() {}

// Return true if the SD card is mounted and ready
bool SDModule::mounted() { return _mounted; }

// Append a timestamped CSV row to the current log file, rotating if needed
void SDModule::logEvent(const char* sensor, const char* json) {
  if (!_mounted || _logPath.isEmpty()) return;

  time_t now = time(nullptr);
  char ts[24];
  if (now > 1704067200UL) {
    struct tm ti;
    gmtime_r(&now, &ti);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &ti);
  } else {
    snprintf(ts, sizeof(ts), "ms/%lu", millis());
  }

  uint32_t rowSize = strlen(ts) + 1 + strlen(sensor) + 1 + strlen(json) + 1;

  if (_maxBytes > 0 && _logBytes + rowSize > _maxBytes) {
    _rotate();
    if (_logPath.isEmpty()) return;
  }

  File f = fs()->open(_logPath, FILE_APPEND);
  if (!f) return;
  f.printf("%s,%s,%s\n", ts, sensor, json);
  f.close();
  _logBytes += rowSize;
}

// Subscribe to temperature, current, and battery EventBus events for SD logging
void SDModule::subscribeEvents() {
  EventBus::subscribe("temperature", [](JsonObject data) {
    String out;
    serializeJson(data, out);
    SDModule::logEvent("temperature", out.c_str());
  });

  EventBus::subscribe("current", [](JsonObject data) {
    String out;
    serializeJson(data, out);
    SDModule::logEvent("current", out.c_str());
  });

  EventBus::subscribe("battery", [](JsonObject data) {
    String out;
    serializeJson(data, out);
    SDModule::logEvent("battery", out.c_str());
  });
}

// Report SD card module status
void SDModule::status(ShellOutput out) {
  fs::FS* sdfs = fs();
  uint64_t total = _spiMode ? SD.totalBytes() : SD_MMC.totalBytes();
  char line[64];
  if (_mounted && total > 0) {
    snprintf(line, sizeof(line), "mounted (%s)  %llu MB",
             _spiMode ? "SPI" : "SD_MMC", (unsigned long long)(total / 1048576));
  } else {
    snprintf(line, sizeof(line), "not mounted");
  }
  out(line);
}

// Run SD card self-test
void SDModule::selftest(ShellOutput out) {
  if (_mounted) {
    out("[PASS] SD card mounted");
  } else {
    out("[WARN] SD card not mounted (optional)");
  }
}

#ifdef ENABLE_SD
MODULE_REGISTER(SDModule, PRIORITY_NETWORK)  // before TFT(SERVICE) to init SPI bus first
#endif
