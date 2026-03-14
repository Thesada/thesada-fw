// thesada-fw — main.cpp
// SPDX-License-Identifier: GPL-3.0-only

#include <Arduino.h>
#include "core/Config.h"
#include "core/ModuleRegistry.h"
#include "core/WiFiManager.h"
#include "core/MQTTClient.h"
#include "core/WebServer.h"
#include "core/ScriptEngine.h"

void setup() {
  Serial.begin(115200);
  Config::load();
  WiFiManager::begin();
  MQTTClient::begin();
  WebServer::begin();
  ScriptEngine::begin();
  ModuleRegistry::begin();
}

void loop() {
  WiFiManager::loop();
  MQTTClient::loop();
  WebServer::loop();
  ModuleRegistry::loop();
}
