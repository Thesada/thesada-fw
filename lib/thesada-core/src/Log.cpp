// thesada-fw - Log.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "Log.h"

void (*Log::_remoteHandler)(const char* line) = nullptr;
char    Log::_ring[RING_SIZE][220] = {};
uint8_t Log::_ringHead = 0;
uint8_t Log::_ringUsed = 0;

// Set a callback for forwarding log lines to a remote transport
void Log::setRemoteHandler(void (*handler)(const char* line)) {
  _remoteHandler = handler;
}

// Retrieve a log line from the ring buffer by index
const char* Log::ringLine(uint8_t index) {
  if (index >= _ringUsed) return nullptr;
  uint8_t pos = (_ringHead + RING_SIZE - _ringUsed + index) % RING_SIZE;
  return _ring[pos];
}

// Return the number of lines currently in the ring buffer
uint8_t Log::ringCount() {
  return _ringUsed;
}

// Format, store, and output a log line to serial and remote handler
void Log::write(LogLevel level, const char* tag, const char* msg) {
  char line[220];
  time_t now = time(nullptr);
  if (now > 1700000000UL) {
    char ts[22];
    struct tm* t = gmtime(&now);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", t);
    snprintf(line, sizeof(line), "[%s][%s][%s] %s", levelStr(level), ts, tag, msg);
  } else {
    snprintf(line, sizeof(line), "[%s][%s] %s", levelStr(level), tag, msg);
  }

  // Store in ring buffer.
  strncpy(_ring[_ringHead], line, sizeof(_ring[0]) - 1);
  _ring[_ringHead][sizeof(_ring[0]) - 1] = '\0';
  _ringHead = (_ringHead + 1) % RING_SIZE;
  if (_ringUsed < RING_SIZE) _ringUsed++;

  Serial.println(line);
  if (_remoteHandler) _remoteHandler(line);
}

// Convert a log level enum to its short string label
const char* Log::levelStr(LogLevel level) {
  switch (level) {
    case LogLevel::DEBUG: return "DBG";
    case LogLevel::INFO:  return "INF";
    case LogLevel::WARN:  return "WRN";
    case LogLevel::ERROR: return "ERR";
    default:              return "???";
  }
}

// Convenience wrappers for each log level
void Log::debug(const char* tag, const char* msg) { write(LogLevel::DEBUG, tag, msg); }
void Log::info(const char* tag, const char* msg)  { write(LogLevel::INFO,  tag, msg); }
void Log::warn(const char* tag, const char* msg)  { write(LogLevel::WARN,  tag, msg); }
void Log::error(const char* tag, const char* msg) { write(LogLevel::ERROR, tag, msg); }
