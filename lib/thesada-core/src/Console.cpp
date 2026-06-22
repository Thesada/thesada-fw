// thesada-fw - Console.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "Console.h"

SemaphoreHandle_t Console::_mutex = nullptr;
Console::Mode     Console::_mode  = Console::Mode::Normal;
uint32_t          Console::_seq   = 0;

void Console::begin() {
  if (!_mutex) _mutex = xSemaphoreCreateMutex();
}

// The lock keeps text + newline together so another task's line can't splice
// in. Unlocked fallback covers writes before begin().
void Console::writeLocked(const char* line) {
  if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
  Serial.println(line);
  if (_mutex) xSemaphoreGive(_mutex);
}

// Mode check and write under one lock: a flip to Command mid-call
// cannot let a log line splice into a command frame.
void Console::log(const char* line) {
  if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
  if (_mode == Mode::Normal) Serial.println(line);
  if (_mutex) xSemaphoreGive(_mutex);
}
void Console::reply(const char* line) { writeLocked(line); }

// Takes the lock directly (not writeLocked) so the _mode read, _seq
// increment, and marker write are one atomic unit; the mutex is
// non-recursive, so calling writeLocked here would deadlock.
void Console::endReply() {
  if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
  if (_mode == Mode::Command) {
    char m[24];
    snprintf(m, sizeof(m), "%s %lu", DONE_MARKER, (unsigned long)_seq++);
    Serial.println(m);
  }
  if (_mutex) xSemaphoreGive(_mutex);
}

void Console::setMode(Mode m) {
  if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY);
  _mode = m;
  if (_mutex) xSemaphoreGive(_mutex);
}

Console::Mode Console::mode() {
  if (!_mutex) return _mode;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  Mode m = _mode;
  xSemaphoreGive(_mutex);
  return m;
}
