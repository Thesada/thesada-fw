// thesada-fw - Log.h
// Shared logging - Serial now, MQTT later
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <thesada_config.h>
#include <Arduino.h>

enum class LogLevel {
  DEBUG,
  INFO,
  WARN,
  ERROR
};

class Log {
public:
  static void debug(const char* tag, const char* msg);
  static void info(const char* tag, const char* msg);
  static void warn(const char* tag, const char* msg);
  static void error(const char* tag, const char* msg);

  // Optional callback for remote log relay (e.g. WebSocket terminal).
  // The formatted line "[INF][Tag] message" is passed to the handler.
  static void setRemoteHandler(void (*handler)(const char* line));

  // Ring buffer: last N log lines kept in memory for replay on WS connect.
  // Defaults here match thesada_config.h; board overrides use #undef/#define.
#ifndef LOG_RING_SIZE
  #define LOG_RING_SIZE 50
#endif
#ifndef LOG_LINE_LEN
  #define LOG_LINE_LEN 220
#endif
  static constexpr uint8_t RING_SIZE = LOG_RING_SIZE;
  static const char* ringLine(uint8_t index);  // 0 = oldest
  static uint8_t     ringCount();

private:
  static void write(LogLevel level, const char* tag, const char* msg);
  static const char* levelStr(LogLevel level);
  static void (*_remoteHandler)(const char* line);

  static char    _ring[RING_SIZE][LOG_LINE_LEN];
  static uint8_t _ringHead;
  static uint8_t _ringUsed;
};
