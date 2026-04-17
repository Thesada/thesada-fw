// thesada-fw - ScriptEngine.h
// Lua 5.3 runtime with bindings to Log, EventBus, MQTT, Config.
// Scripts live on LittleFS: main.lua (runs once at boot), rules.lua (event-driven, hot-reloadable).
// Modules register their own Lua bindings via addBindings() in their begin().
// The registrar list is persistent - survives lua.reload.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>

// Forward declare lua_State so modules don't need Lua headers
struct lua_State;

// Callback type for module Lua binding registration
using LuaBindingRegistrar = void(*)(lua_State*);

class ScriptEngine {
public:
  static void begin();
  static void loop();
  static void reload();
  static uint32_t generation();

  // Modules call this in their begin() to register Lua bindings.
  // Registrars are stored persistently and re-called on lua.reload.
  static void addBindings(LuaBindingRegistrar fn);

private:
  static void createState();
  static void destroyState();
  static void registerCoreBindings();
  static void callAllRegistrars();
  static bool executeFile(const char* path);

  static uint32_t _generation;
  static constexpr uint8_t MAX_REGISTRARS = 16;
  static LuaBindingRegistrar _registrars[MAX_REGISTRARS];
  static uint8_t _registrarCount;
};
