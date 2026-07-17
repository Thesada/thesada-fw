// thesada-fw - Log.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "Log.h"
#include "Console.h"
#include "log_kv_policy.h"

void (*Log::_remoteHandler)(const char* line) = nullptr;
char    Log::_ring[RING_SIZE][LOG_LINE_LEN] = {};
uint8_t Log::_ringHead = 0;
uint8_t Log::_ringUsed = 0;

void Log::setRemoteHandler(void (*handler)(const char* line)) {
  _remoteHandler = handler;
}

const char* Log::ringLine(uint8_t index) {
  if (index >= _ringUsed) return nullptr;
  uint8_t pos = (_ringHead + RING_SIZE - _ringUsed + index) % RING_SIZE;
  return _ring[pos];
}

uint8_t Log::ringCount() {
  return _ringUsed;
}

void Log::write(LogLevel level, const char* tag, const char* msg) {
  char line[LOG_LINE_LEN];
  time_t now = time(nullptr);
  // Timestamp only once the clock is set; pre-NTP epoch makes every
  // line read as 1970, so omit it until time() clears the sentinel.
  if (now > 1700000000UL) {
    char ts[22];
    struct tm* t = gmtime(&now);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", t);
    snprintf(line, sizeof(line), "[%s][%s][%s] %s", levelStr(level), ts, tag, msg);
  } else {
    snprintf(line, sizeof(line), "[%s][%s] %s", levelStr(level), tag, msg);
  }

  strncpy(_ring[_ringHead], line, sizeof(_ring[0]) - 1);
  _ring[_ringHead][sizeof(_ring[0]) - 1] = '\0';
  _ringHead = (_ringHead + 1) % RING_SIZE;
  if (_ringUsed < RING_SIZE) _ringUsed++;

  Console::log(line);
  if (_remoteHandler) _remoteHandler(line);
}

const char* Log::levelStr(LogLevel level) {
  switch (level) {
    case LogLevel::DEBUG: return "DBG";
    case LogLevel::INFO:  return "INF";
    case LogLevel::WARN:  return "WRN";
    case LogLevel::ERROR: return "ERR";
    default:              return "???";
  }
}

void Log::debug(const char* tag, const char* msg) { write(LogLevel::DEBUG, tag, msg); }
void Log::info(const char* tag, const char* msg)  { write(LogLevel::INFO,  tag, msg); }
void Log::warn(const char* tag, const char* msg)  { write(LogLevel::WARN,  tag, msg); }
void Log::error(const char* tag, const char* msg) { write(LogLevel::ERROR, tag, msg); }

void Log::vkvf(LogLevel level, const char* tag, const char* fmt, va_list ap) {
  char msg[LOG_LINE_LEN];
  logKvFormatV(msg, sizeof(msg), fmt, ap);
  write(level, tag, msg);
}

void Log::kvf(const char* tag, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vkvf(LogLevel::INFO, tag, fmt, ap);
  va_end(ap);
}

void Log::kvfw(const char* tag, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vkvf(LogLevel::WARN, tag, fmt, ap);
  va_end(ap);
}

void Log::kvfe(const char* tag, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vkvf(LogLevel::ERROR, tag, fmt, ap);
  va_end(ap);
}
