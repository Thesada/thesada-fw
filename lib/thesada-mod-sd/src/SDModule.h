// thesada-fw - SDModule.h
// SD card data logger. Supports both SD_MMC (LILYGO) and SPI (CYD) modes.
// Mode is selected via sd.mode in config.json: "sdmmc" (default) or "spi".
// Subscribes to EventBus events and appends CSV rows to the current log file.
// Log files are named /log001.csv, /log002.csv, ... On boot, resume the
// highest-numbered existing file if it is under sd.max_file_kb; otherwise
// open a fresh slot. Rotation is size-triggered only (not per-boot).
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <FS.h>
#include <Module.h>

class SDModule : public Module {
public:
  void begin() override;
  void loop() override;
  const char* name() override { return "SDModule"; }
  const char* configKey() override { return "sd"; }
  void status(ShellOutput out) override;
  void selftest(ShellOutput out) override;

  static bool mounted();
  // Append one CSV row: timestamp,sensor,json_data
  static void logEvent(const char* sensor, const char* json);
  // Get the FS pointer (SD_MMC or SD depending on mode) for file access
  static fs::FS* fs();

private:
  void subscribeEvents();
  static bool     _mounted;
  static bool     _spiMode;
  static String   _logPath;
  static uint32_t _logBytes;
  static uint32_t _maxBytes;

  static bool _openNextLog();
  static bool _resumeOrOpen();
  static void _rotate();

  // fs.df probes registered with Shell::registerFS. Static so they match
  // the FSDfFn (uint64_t(*)()) pointer type; pick SD vs SD_MMC by _spiMode.
  static uint64_t _dfUsed();
  static uint64_t _dfTotal();
};
