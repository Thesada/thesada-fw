// thesada-fw - Console.h
// One serial-console output path. Log lines, shell responses, and boot prints
// all go through here under a single mutex, so concurrent writers can no longer
// byte-interleave into mangled lines.
//
// Mode (resets to Normal on reboot): Normal mirrors logs to serial; Command
// keeps logs off serial (ring buffer + WS handler still receive them) and ends
// each response with DONE_MARKER + a sequence number, so an automated reader
// frames command output deterministically.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class Console {
public:
  enum class Mode : uint8_t { Normal, Command };

  // Call once in setup() after Serial.begin(). Writes before it run unlocked.
  static void begin();

  static void log(const char* line);     // log line; reaches serial only in Normal
  static void reply(const char* line);   // command response; always printed
  static void endReply();                // emit "DONE_MARKER <seq>"; Command only
  static void setMode(Mode m);
  static Mode mode();

  // Split literal: "\x04CMD-DONE" would fold the 'C' into the hex escape.
  static constexpr const char* DONE_MARKER = "\x04" "CMD-DONE";

private:
  static void writeLocked(const char* line);
  static SemaphoreHandle_t _mutex;
  static Mode              _mode;
  static uint32_t          _seq;
};
