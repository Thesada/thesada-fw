// thesada-fw — Config.h
// Loads config.json from LittleFS and exposes values
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <ArduinoJson.h>

class Config {
public:
  static void load();
  static JsonObject get();
private:
  static StaticJsonDocument<2048> _doc;
};
