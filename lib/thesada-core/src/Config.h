// thesada-fw - Config.h
// Loads config.json from LittleFS and exposes values
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <ArduinoJson.h>

class Config {
public:
  static void load();
  static void save();
  static void replace(const char* json);
  static bool set(const char* path, const char* value);
  static JsonObject get();
private:
  static JsonDocument _doc;
};
