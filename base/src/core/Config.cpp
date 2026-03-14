// thesada-fw — Config.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "Config.h"
#include <LittleFS.h>

StaticJsonDocument<2048> Config::_doc;

void Config::load() {
  if (!LittleFS.begin()) return;
  File f = LittleFS.open("/config.json", "r");
  if (!f) return;
  deserializeJson(_doc, f);
  f.close();
}

JsonObject Config::get() {
  return _doc.as<JsonObject>();
}
