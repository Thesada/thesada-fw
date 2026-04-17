// thesada-fw - ScriptEngine.cpp
// Lua 5.3 scripting engine with bindings for Log, EventBus, MQTT, Config.
//
#include <thesada_config.h>
#ifdef ENABLE_SCRIPTENGINE
//
// Scripts on LittleFS:
//   /scripts/main.lua   - runs once at boot (setup tasks, one-time logic)
//   /scripts/rules.lua  - event-driven rules (hot-reloadable via reload())
//
// Lua API:
//   Log.info(msg)                      - log at INFO level (tag: "Lua")
//   Log.warn(msg)                      - log at WARN level
//   Log.error(msg)                     - log at ERROR level
//   EventBus.subscribe(event, func)    - subscribe to named event, func receives a table
//   MQTT.publish(topic, payload)       - publish a message to MQTT
//   Config.get(key)                    - read config value by dot-notation key
//   Node.restart()                     - reboot the device
//   Node.version()                     - returns FIRMWARE_VERSION string
//   Node.uptime()                      - returns millis() as number
//
// Hot reload:
//   ScriptEngine::reload() bumps generation counter, destroys Lua state,
//   creates fresh state, re-executes both scripts. Stale EventBus callbacks
//   check generation and silently skip.
//
// SPDX-License-Identifier: GPL-3.0-only

#include "ScriptEngine.h"
#include <Config.h>
#include <EventBus.h>
#include <MQTTClient.h>
#include <Log.h>
#include <Shell.h>
#include <ModuleRegistry.h>

#include <LittleFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>

extern "C" {
#include <lua/lua.h>
#include <lua/lualib.h>
#include <lua/lauxlib.h>
}

static const char* TAG = "Lua";

lua_State* gL = nullptr;
uint32_t ScriptEngine::_generation = 0;
LuaBindingRegistrar ScriptEngine::_registrars[ScriptEngine::MAX_REGISTRARS] = {};
uint8_t ScriptEngine::_registrarCount = 0;

// ---------------------------------------------------------------------------
// Lua binding: Log
// ---------------------------------------------------------------------------

// Lua binding: Log.info(msg)
static int lua_log_info(lua_State* L) {
  const char* msg = luaL_checkstring(L, 1);
  Log::info("Lua", msg);
  return 0;
}

// Lua binding: Log.warn(msg)
static int lua_log_warn(lua_State* L) {
  const char* msg = luaL_checkstring(L, 1);
  Log::warn("Lua", msg);
  return 0;
}

// Lua binding: Log.error(msg)
static int lua_log_error(lua_State* L) {
  const char* msg = luaL_checkstring(L, 1);
  Log::error("Lua", msg);
  return 0;
}

static const luaL_Reg logLib[] = {
  {"info",  lua_log_info},
  {"warn",  lua_log_warn},
  {"error", lua_log_error},
  {nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// Lua binding: MQTT
// ---------------------------------------------------------------------------

// Lua binding: MQTT.publish(topic, payload)
// Publish a message to an MQTT topic
static int lua_mqtt_publish(lua_State* L) {
  const char* topic   = luaL_checkstring(L, 1);
  const char* payload = luaL_checkstring(L, 2);
  MQTTClient::publish(topic, payload);
  return 0;
}

// Subscribe to an MQTT topic with a Lua callback(topic, payload)
static int lua_mqtt_subscribe(lua_State* L) {
  const char* topic = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  lua_pushvalue(L, 2);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);
  uint32_t gen = ScriptEngine::generation();

  MQTTClient::subscribe(topic, [ref, gen](const char* topic, const char* payload) {
    if (!gL || ScriptEngine::generation() != gen) return;
    lua_rawgeti(gL, LUA_REGISTRYINDEX, ref);
    lua_pushstring(gL, topic);
    lua_pushstring(gL, payload);
    if (lua_pcall(gL, 2, 0, 0) != LUA_OK) {
      const char* err = lua_tostring(gL, -1);
      char msg[128];
      snprintf(msg, sizeof(msg), "MQTT callback error: %.100s", err ? err : "unknown");
      Log::error("Lua", msg);
      lua_pop(gL, 1);
    }
  });

  char msg[96];
  snprintf(msg, sizeof(msg), "Lua subscribed to MQTT: %s", topic);
  Log::info("Lua", msg);
  return 0;
}

static const luaL_Reg mqttLib[] = {
  {"publish",   lua_mqtt_publish},
  {"subscribe", lua_mqtt_subscribe},
  {nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// Lua binding: Node
// ---------------------------------------------------------------------------

// Timer queue for Node.setTimeout
struct LuaTimer {
  uint32_t fireAt;
  int callbackRef;  // LUA_NOREF if unused
};
static constexpr int MAX_TIMERS = 8;
static LuaTimer _timers[MAX_TIMERS] = {};

// Lua binding: Node.setTimeout(ms, callback) - queue a one-shot timer
static int lua_node_setTimeout(lua_State* L) {
  int ms = (int)luaL_checknumber(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  lua_pushvalue(L, 2);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);
  uint32_t fireAt = millis() + (uint32_t)ms;

  for (int i = 0; i < MAX_TIMERS; i++) {
    if (_timers[i].callbackRef == LUA_NOREF) {
      _timers[i] = { fireAt, ref };
      lua_pushboolean(L, true);
      return 1;
    }
  }
  luaL_unref(L, LUA_REGISTRYINDEX, ref);
  Log::warn("Lua", "Timer queue full (max 8)");
  lua_pushboolean(L, false);
  return 1;
}

// Lua binding: Node.restart() - reboot the device
static int lua_node_restart(lua_State* L) {
  Log::info("Lua", "Restart requested from script");
  delay(100);
  ESP.restart();
  return 0;
}

// Lua binding: Node.version() - return firmware version string
static int lua_node_version(lua_State* L) {
  lua_pushstring(L, FIRMWARE_VERSION);
  return 1;
}

// Lua binding: Node.uptime() - return millis since boot
static int lua_node_uptime(lua_State* L) {
  lua_pushnumber(L, (lua_Number)millis());
  return 1;
}

// Lua binding: Node.ip() - return the WiFi local IP address
static int lua_node_ip(lua_State* L) {
  lua_pushstring(L, WiFi.localIP().toString().c_str());
  return 1;
}

static const luaL_Reg nodeLib[] = {
  {"restart",    lua_node_restart},
  {"version",    lua_node_version},
  {"uptime",     lua_node_uptime},
  {"ip",         lua_node_ip},
  {"setTimeout", lua_node_setTimeout},
  {nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// Lua binding: Config
// ---------------------------------------------------------------------------

// Lua binding: Config.get(key) - read config value by dot-notation key
static int lua_config_get(lua_State* L) {
  const char* key = luaL_checkstring(L, 1);
  JsonObject cfg = Config::get();

  JsonVariant current = cfg;  // JsonObject implicitly converts to JsonVariant in ArduinoJson v7
  char buf[128];
  strncpy(buf, key, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char* token = strtok(buf, ".");
  while (token != nullptr) {
    if (current.is<JsonObject>()) {
      current = current[token];
    } else if (current.is<JsonArray>()) {
      // Try numeric index for arrays (e.g. "chat_ids.0")
      char* end;
      long idx = strtol(token, &end, 10);
      if (*end == '\0') {
        current = current[(size_t)idx];
      } else {
        lua_pushnil(L);
        return 1;
      }
    } else {
      lua_pushnil(L);
      return 1;
    }
    token = strtok(nullptr, ".");
  }

  if (current.isNull()) {
    lua_pushnil(L);
  } else if (current.is<bool>()) {
    lua_pushboolean(L, current.as<bool>());
  } else if (current.is<int>()) {
    lua_pushinteger(L, current.as<int>());
  } else if (current.is<float>()) {
    lua_pushnumber(L, current.as<float>());
  } else if (current.is<const char*>()) {
    lua_pushstring(L, current.as<const char*>());
  } else {
    String json;
    serializeJson(current, json);
    lua_pushstring(L, json.c_str());
  }
  return 1;
}

static const luaL_Reg configLib[] = {
  {"get", lua_config_get},
  {nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// Lua binding: EventBus
// ---------------------------------------------------------------------------

// Recursively convert an ArduinoJson object to a Lua table on the stack
static void jsonObjectToLuaTable(lua_State* L, JsonObject obj) {
  lua_newtable(L);
  for (JsonPair kv : obj) {
    lua_pushstring(L, kv.key().c_str());
    JsonVariant v = kv.value();
    if (v.is<bool>()) {
      lua_pushboolean(L, v.as<bool>());
    } else if (v.is<long>()) {
      lua_pushinteger(L, v.as<long>());
    } else if (v.is<double>()) {
      lua_pushnumber(L, v.as<double>());
    } else if (v.is<const char*>()) {
      lua_pushstring(L, v.as<const char*>());
    } else if (v.is<JsonArray>()) {
      JsonArray arr = v.as<JsonArray>();
      lua_newtable(L);
      int idx = 1;
      for (JsonVariant elem : arr) {
        lua_pushinteger(L, idx++);
        if (elem.is<JsonObject>()) {
          jsonObjectToLuaTable(L, elem.as<JsonObject>());
        } else if (elem.is<long>()) {
          lua_pushinteger(L, elem.as<long>());
        } else if (elem.is<double>()) {
          lua_pushnumber(L, elem.as<double>());
        } else if (elem.is<const char*>()) {
          lua_pushstring(L, elem.as<const char*>());
        } else {
          String json;
          serializeJson(elem, json);
          lua_pushstring(L, json.c_str());
        }
        lua_settable(L, -3);
      }
    } else if (v.is<JsonObject>()) {
      jsonObjectToLuaTable(L, v.as<JsonObject>());
    } else {
      String json;
      serializeJson(v, json);
      lua_pushstring(L, json.c_str());
    }
    lua_settable(L, -3);
  }
}

// Lua binding: EventBus.subscribe(event, callback) - generation-aware
static int lua_eventbus_subscribe(lua_State* L) {
  const char* event = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);

  lua_pushvalue(L, 2);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);

  uint32_t gen = ScriptEngine::generation();

  EventBus::subscribe(event, [gen, ref](JsonObject data) {
    if (gen != ScriptEngine::generation()) return;
    if (gL == nullptr) return;

    lua_rawgeti(gL, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(gL, -1)) {
      lua_pop(gL, 1);
      return;
    }

    jsonObjectToLuaTable(gL, data);
    if (lua_pcall(gL, 1, 0, 0) != LUA_OK) {
      const char* err = lua_tostring(gL, -1);
      char msg[128];
      snprintf(msg, sizeof(msg), "Callback error: %.100s", err ? err : "unknown");
      Log::error("Lua", msg);
      lua_pop(gL, 1);
    }
  });

  char msg[64];
  snprintf(msg, sizeof(msg), "Subscribed to event: %s", event);
  Log::info(TAG, msg);
  return 0;
}

static const luaL_Reg eventbusLib[] = {
  {"subscribe", lua_eventbus_subscribe},
  {nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// Lua binding: JSON
// ---------------------------------------------------------------------------

// Parse a JSON string into a Lua table
static int lua_json_decode(lua_State* L) {
  const char* str = luaL_checkstring(L, 1);
  JsonDocument doc;
  if (deserializeJson(doc, str)) {
    lua_pushnil(L);
    return 1;
  }
  if (doc.is<JsonObject>()) {
    jsonObjectToLuaTable(L, doc.as<JsonObject>());
  } else if (doc.is<JsonArray>()) {
    JsonArray arr = doc.as<JsonArray>();
    lua_newtable(L);
    int idx = 1;
    for (JsonVariant elem : arr) {
      lua_pushinteger(L, idx++);
      if (elem.is<JsonObject>()) {
        jsonObjectToLuaTable(L, elem.as<JsonObject>());
      } else if (elem.is<double>()) {
        lua_pushnumber(L, elem.as<double>());
      } else if (elem.is<const char*>()) {
        lua_pushstring(L, elem.as<const char*>());
      } else {
        lua_pushnil(L);
      }
      lua_settable(L, -3);
    }
  } else {
    lua_pushnil(L);
  }
  return 1;
}

static const luaL_Reg jsonLib[] = {
  {"decode", lua_json_decode},
  {nullptr, nullptr}
};

// Display, TFT, and Telegram Lua bindings have moved to their respective
// module .cpp files. They register via ScriptEngine::addBindings() in begin().

// ---------------------------------------------------------------------------
// State management
// ---------------------------------------------------------------------------

// Create a fresh Lua state and register all native bindings
void ScriptEngine::createState() {
  // Clear pending timers from previous state.
  for (int i = 0; i < MAX_TIMERS; i++) _timers[i].callbackRef = LUA_NOREF;

  gL = luaL_newstate();
  if (!gL) {
    Log::error(TAG, "Failed to create Lua state - out of memory?");
    return;
  }

  luaL_openlibs(gL);

  // ESP-Arduino-Lua may not fully load all standard libs. Ensure they exist.
  luaL_requiref(gL, "math",   luaopen_math,   1); lua_pop(gL, 1);
  luaL_requiref(gL, "table",  luaopen_table,  1); lua_pop(gL, 1);
  luaL_requiref(gL, "string", luaopen_string, 1); lua_pop(gL, 1);
  luaL_requiref(gL, "io",     luaopen_io,     1); lua_pop(gL, 1);

  callAllRegistrars();
  Log::info(TAG, "Lua state created");
}

// Close and free the current Lua state
void ScriptEngine::destroyState() {
  if (gL) {
    lua_close(gL);
    gL = nullptr;
    Log::info(TAG, "Lua state destroyed");
  }
}

// Register core Lua bindings (Log, MQTT, Node, Config, EventBus, JSON)
void ScriptEngine::registerCoreBindings() {
  if (!gL) return;

  luaL_newlib(gL, logLib);
  lua_setglobal(gL, "Log");

  luaL_newlib(gL, mqttLib);
  lua_setglobal(gL, "MQTT");

  luaL_newlib(gL, nodeLib);
  lua_setglobal(gL, "Node");

  luaL_newlib(gL, configLib);
  lua_setglobal(gL, "Config");

  luaL_newlib(gL, eventbusLib);
  lua_setglobal(gL, "EventBus");

  luaL_newlib(gL, jsonLib);
  lua_setglobal(gL, "JSON");
}

// Register a module's Lua binding function (called in module begin())
void ScriptEngine::addBindings(LuaBindingRegistrar fn) {
  if (_registrarCount >= MAX_REGISTRARS) return;
  _registrars[_registrarCount++] = fn;
}

// Call all registered binding functions (core + module)
void ScriptEngine::callAllRegistrars() {
  registerCoreBindings();
  for (uint8_t i = 0; i < _registrarCount; i++) {
    if (_registrars[i]) _registrars[i](gL);
  }
}

// Load and execute a Lua script file from LittleFS
bool ScriptEngine::executeFile(const char* path) {
  if (!gL) return false;

  if (!LittleFS.exists(path)) {
    char msg[64];
    snprintf(msg, sizeof(msg), "%s not found - skipping", path);
    Log::info(TAG, msg);
    return false;
  }

  File f = LittleFS.open(path, "r");
  if (!f) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Failed to open %s", path);
    Log::error(TAG, msg);
    return false;
  }

  String script = f.readString();
  f.close();

  int result = luaL_dostring(gL, script.c_str());
  if (result != LUA_OK) {
    const char* err = lua_tostring(gL, -1);
    char msg[196];
    snprintf(msg, sizeof(msg), "%s error: %.160s", path, err ? err : "unknown");
    Log::error(TAG, msg);
    lua_pop(gL, 1);
    return false;
  }

  char msg[64];
  snprintf(msg, sizeof(msg), "%s executed", path);
  Log::info(TAG, msg);
  return true;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

// Initialize Lua, run startup scripts, and subscribe to MQTT reload trigger
void ScriptEngine::begin() {
  createState();
  if (!gL) return;

  executeFile("/scripts/main.lua");
  executeFile("/scripts/rules.lua");

  // Subscribe to MQTT reload trigger.
  JsonObject cfg = Config::get();
  const char* customTopic = cfg["lua"]["reload_topic"] | "";
  char topic[96];
  if (strlen(customTopic) > 0) {
    strncpy(topic, customTopic, sizeof(topic) - 1);
    topic[sizeof(topic) - 1] = '\0';
  } else {
    const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
    snprintf(topic, sizeof(topic), "%s/cmd/lua/reload", prefix);
  }

  MQTTClient::subscribe(topic, [](const char* topic, const char* payload) {
    Log::info(TAG, "MQTT Lua reload trigger received");
    ScriptEngine::reload();
  });

  char msg[128];
  snprintf(msg, sizeof(msg), "Ready - reload via MQTT: %s", topic);
  Log::info(TAG, msg);

  // Register Lua shell commands
  Shell::registerCommand("lua.exec", "Execute inline Lua code",
      [](int argc, char** argv, ShellOutput out) {
        if (argc < 2) { out("Usage: lua.exec <code>"); return; }
        extern lua_State* gL;
        if (!gL) { out("Lua state not initialized"); return; }
        String code;
        for (int i = 1; i < argc; i++) {
          if (i > 1) code += " ";
          code += argv[i];
        }
        int result = luaL_dostring(gL, code.c_str());
        if (result != LUA_OK) {
          const char* err = lua_tostring(gL, -1);
          char msg[196];
          snprintf(msg, sizeof(msg), "Error: %.180s", err ? err : "unknown");
          out(msg);
          lua_pop(gL, 1);
        } else {
          if (lua_gettop(gL) > 0) {
            const char* r = lua_tostring(gL, -1);
            if (r) out(r);
            lua_pop(gL, lua_gettop(gL));
          } else {
            out("OK");
          }
        }
      });

  Shell::registerCommand("lua.load", "Execute a Lua file from LittleFS",
      [](int argc, char** argv, ShellOutput out) {
        if (argc < 2) { out("Usage: lua.load <path>"); return; }
        extern lua_State* gL;
        if (!gL) { out("Lua state not initialized"); return; }
        if (!LittleFS.exists(argv[1])) {
          char msg[64];
          snprintf(msg, sizeof(msg), "%s not found", argv[1]);
          out(msg);
          return;
        }
        File f = LittleFS.open(argv[1], "r");
        if (!f) { out("Failed to open file"); return; }
        String script = f.readString();
        f.close();
        int result = luaL_dostring(gL, script.c_str());
        if (result != LUA_OK) {
          const char* err = lua_tostring(gL, -1);
          char msg[196];
          snprintf(msg, sizeof(msg), "Error: %.180s", err ? err : "unknown");
          out(msg);
          lua_pop(gL, 1);
        } else {
          char msg[64];
          snprintf(msg, sizeof(msg), "%s executed", argv[1]);
          out(msg);
        }
      });

  Shell::registerCommand("lua.reload", "Hot-reload Lua scripts",
      [](int argc, char** argv, ShellOutput out) {
        ScriptEngine::reload();
        bool hasMain  = LittleFS.exists("/scripts/main.lua");
        bool hasRules = LittleFS.exists("/scripts/rules.lua");
        char msg[96];
        snprintf(msg, sizeof(msg), "Lua reloaded (main.lua=%s, rules.lua=%s)",
                 hasMain ? "yes" : "no", hasRules ? "yes" : "no");
        out(msg);
      });
}

// Hot-reload scripts by bumping generation and re-creating the Lua state
void ScriptEngine::reload() {
  Log::info(TAG, "Reloading scripts...");
  _generation++;

  destroyState();
  createState();
  if (!gL) return;

  executeFile("/scripts/main.lua");
  executeFile("/scripts/rules.lua");

  char msg[64];
  snprintf(msg, sizeof(msg), "Reloaded (generation %lu)", _generation);
  Log::info(TAG, msg);
}

// Return the current script generation counter
uint32_t ScriptEngine::generation() {
  return _generation;
}

// Fire any expired Lua timers registered via Node.setTimeout
void ScriptEngine::loop() {
  if (!gL) return;
  uint32_t now = millis();
  for (int i = 0; i < MAX_TIMERS; i++) {
    if (_timers[i].callbackRef != LUA_NOREF && now >= _timers[i].fireAt) {
      int ref = _timers[i].callbackRef;
      _timers[i].callbackRef = LUA_NOREF;
      lua_rawgeti(gL, LUA_REGISTRYINDEX, ref);
      if (lua_pcall(gL, 0, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(gL, -1);
        char msg[128];
        snprintf(msg, sizeof(msg), "Timer error: %.100s", err ? err : "unknown");
        Log::error("Lua", msg);
        lua_pop(gL, 1);
      }
      luaL_unref(gL, LUA_REGISTRYINDEX, ref);
    }
  }
}

// Module wrapper for self-registration
class ScriptEngineModule : public Module {
public:
  // Delegate to static ScriptEngine::begin
  void begin() override { ScriptEngine::begin(); }
  // Delegate to static ScriptEngine::loop
  void loop() override { ScriptEngine::loop(); }
  // Return module name
  const char* name() override { return "ScriptEngine"; }
  // Check Lua state health
  void selftest(ShellOutput out) override {
    extern lua_State* gL;
    if (gL) { out("[PASS] Lua state active"); }
    else { out("[FAIL] Lua state not initialized"); }
  }
};

MODULE_REGISTER(ScriptEngineModule, PRIORITY_SCRIPT)

#endif // ENABLE_SCRIPTENGINE
