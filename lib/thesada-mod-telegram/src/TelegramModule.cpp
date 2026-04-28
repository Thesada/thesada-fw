// thesada-fw - TelegramModule.cpp
// Telegram Bot API and webhook integration.
// Alert logic has moved to Lua scripts (data/scripts/alerts.lua).
// This module only provides the send functions:
//
#include <thesada_config.h>
#ifdef ENABLE_TELEGRAM
//
//   TelegramModule::send(message)          - send to all configured chat_ids
//   TelegramModule::sendTo(chatId, message) - send to a specific chat_id
// Both also publish to EventBus("alert") for MQTT and cellular relay.
// SPDX-License-Identifier: GPL-3.0-only
#include "TelegramModule.h"
#include <Config.h>
#include <EventBus.h>
#include <WiFiManager.h>
#include <MQTTClient.h>
#include <Log.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#ifdef ENABLE_SCRIPTENGINE
#include <ScriptEngine.h>
extern "C" {
#include <lua/lua.h>
#include <lua/lualib.h>
#include <lua/lauxlib.h>
}
#endif
#include <ModuleRegistry.h>

static const char* TAG = "Telegram";

bool TelegramModule::_ready = false;
WiFiClientSecure TelegramModule::_secClient;

// ---------------------------------------------------------------------------

// Mark module as ready and log the bot token and chat count
void TelegramModule::begin() {
  _ready = true;

  JsonObject cfg    = Config::get();
  const char* token = cfg["telegram"]["bot_token"] | "";
  JsonVariant chatIds = cfg["telegram"]["chat_ids"];
  int nChats = 0;
  if (chatIds.is<JsonArray>()) nChats = (int)chatIds.as<JsonArray>().size();
  else if (chatIds.is<JsonObject>()) nChats = (int)chatIds.as<JsonObject>().size();
  bool hasToken = strlen(token) > 0;

  // Configure the persistent TLS client once. This instance is never
  // deleted - avoids the heap fragmentation that kills Telegram after 3 alerts.
  _secClient.setInsecure();
  _secClient.setTimeout(10);

  char msg[64];
  snprintf(msg, sizeof(msg), "Ready - token=%s, %d chat(s)", hasToken ? "yes" : "no", nChats);
  Log::info(TAG, msg);

  // Register Lua bindings (Telegram.send, Telegram.broadcast)
  #ifdef ENABLE_SCRIPTENGINE
  ScriptEngine::addBindings([](lua_State* L) {
    static const luaL_Reg telegramLib[] = {
      {"send", [](lua_State* L) -> int {
        bool ok = TelegramModule::sendTo(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
        lua_pushboolean(L, ok);
        return 1;
      }},
      {"broadcast", [](lua_State* L) -> int {
        bool ok = TelegramModule::send(luaL_checkstring(L, 1));
        lua_pushboolean(L, ok);
        return 1;
      }},
      {nullptr, nullptr}
    };
    luaL_newlib(L, telegramLib);
    lua_setglobal(L, "Telegram");
  });
  #endif
}

// ---------------------------------------------------------------------------

// No periodic work - sends are triggered externally
void TelegramModule::loop() {}

// ---------------------------------------------------------------------------

// Send a message to a specific Telegram chat ID via the Bot API.
// Skips the send if free heap is below TELEGRAM_HEAP_FLOOR_BYTES so the
// TLS handshake here cannot starve the MQTT client of buffer space.
// MQTT has hard priority because it carries OTA. (Forgejo #40 Phase 3)
bool TelegramModule::sendTo(const char* chatId, const char* message) {
  if (!chatId || strlen(chatId) == 0 || !message || strlen(message) == 0) return false;
  if (!WiFiManager::connected()) { Log::warn(TAG, "WiFi down - skipping"); return false; }

  if (TELEGRAM_HEAP_FLOOR_BYTES > 0) {
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < TELEGRAM_HEAP_FLOOR_BYTES) {
      char wmsg[96];
      snprintf(wmsg, sizeof(wmsg),
               "heap %lu B below floor %d B - skipping (MQTT priority)",
               (unsigned long)freeHeap, (int)TELEGRAM_HEAP_FLOOR_BYTES);
      Log::warn(TAG, wmsg);
      return false;
    }
  }

  JsonObject cfg    = Config::get();
  const char* token = cfg["telegram"]["bot_token"] | "";
  if (strlen(token) == 0) { Log::warn(TAG, "No bot_token"); return false; }

  char url[128];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", token);

  JsonDocument tdoc;
  tdoc["chat_id"] = chatId;
  tdoc["text"]    = message;
  String body;
  serializeJson(tdoc, body);

  // Reuse the persistent TLS client. stop() closes the TCP socket but
  // keeps the mbedtls context allocated - no new heap allocation needed.
  _secClient.stop();

  HTTPClient http;
  http.begin(_secClient, url);
  http.setTimeout(10000);  // 10s HTTP timeout
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  http.end();

  if (code == 200) {
    char smsg[64];
    snprintf(smsg, sizeof(smsg), "Sent to %s", chatId);
    Log::info(TAG, smsg);
    return true;
  } else {
    char emsg[64];
    snprintf(emsg, sizeof(emsg), "HTTP %d for %s", code, chatId);
    Log::warn(TAG, emsg);
    return false;
  }
}

// ---------------------------------------------------------------------------

// Broadcast a message to all configured chat IDs, EventBus, and optional webhook.
// Appends `[heap=N]` to the outbound message so every alert carries a heap
// breadcrumb. MQTT+TLS+Telegram+TLS concurrency is the most likely failure
// mode for this device class, and the heap value at the moment of alert is
// the single most useful datapoint for diagnosing it.
bool TelegramModule::send(const char* message) {
  if (!_ready) { Log::warn(TAG, "not ready"); return false; }

  char tagged[256];
  snprintf(tagged, sizeof(tagged), "%s [heap=%lu]",
           message, (unsigned long)MQTTClient::currentFreeHeap());

  // Publish alert event - MQTTClient and CellularModule subscribe to this.
  JsonDocument doc;
  doc["value"] = tagged;
  EventBus::publish("alert", doc.as<JsonObject>());

  // Send to all configured chat_ids (supports both array and object format).
  JsonObject cfg    = Config::get();
  JsonVariant ids   = cfg["telegram"]["chat_ids"];
  if (ids.is<JsonArray>()) {
    for (const char* chatId : ids.as<JsonArray>()) {
      sendTo(chatId, message);
    }
  } else if (ids.is<JsonObject>()) {
    for (JsonPair kv : ids.as<JsonObject>()) {
      const char* chatId = kv.value().as<const char*>();
      if (chatId && strlen(chatId) > 0) sendTo(chatId, message);
    }
  }

  // Optional HTTP webhook.
  {
    const char* url = cfg["webhook"]["url"] | "";
    if (strlen(url) > 0 && WiFiManager::connected()) {
      const char* tmpl = cfg["webhook"]["message_template"] | "{{value}}";
      String wbody = String(tmpl);
      wbody.replace("{{value}}", message);

      JsonDocument wdoc;
      wdoc["value"] = wbody;
      String postBody;
      serializeJson(wdoc, postBody);

      HTTPClient http;
      bool isHttps = String(url).startsWith("https");
      WiFiClient pc;
      if (isHttps) {
        _secClient.stop();
        http.begin(_secClient, url);
      } else {
        pc.setTimeout(10);
        http.begin(pc, url);
      }
      http.setTimeout(10000);  // 10s HTTP timeout
      http.addHeader("Content-Type", "application/json");
      int code = http.POST(postBody);
      if (code > 0) {
        char wmsg[48];
        snprintf(wmsg, sizeof(wmsg), "Webhook HTTP %d", code);
        Log::info(TAG, wmsg);
      } else {
        Log::warn(TAG, "Webhook POST failed");
      }
      http.end();
    }
  }

  return true;
}

// Report Telegram module status
void TelegramModule::status(ShellOutput out) {
  JsonObject cfg = Config::get();
  const char* token = cfg["telegram"]["bot_token"] | "";
  JsonArray chatIds = cfg["telegram"]["chat_ids"].as<JsonArray>();
  int nChats = chatIds.isNull() ? 0 : (int)chatIds.size();
  char line[96];
  snprintf(line, sizeof(line), "token=%s  %d chat(s)", strlen(token) > 0 ? "yes" : "no", nChats);
  out(line);
}

MODULE_REGISTER(TelegramModule, PRIORITY_SERVICE)  // before ScriptEngine(40) so Lua bindings are registered first

#endif // ENABLE_TELEGRAM
