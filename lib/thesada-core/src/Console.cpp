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

void Console::log(const char* line)   { if (_mode == Mode::Normal) writeLocked(line); }
void Console::reply(const char* line) { writeLocked(line); }

void Console::endReply() {
  if (_mode != Mode::Command) return;
  char m[24];
  snprintf(m, sizeof(m), "%s %lu", DONE_MARKER, (unsigned long)_seq++);
  writeLocked(m);
}

void Console::setMode(Mode m) { _mode = m; }
Console::Mode Console::mode() { return _mode; }
