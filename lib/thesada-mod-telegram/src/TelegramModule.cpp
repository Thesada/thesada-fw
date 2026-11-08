// thesada-fw - TelegramModule.cpp
// Telegram Bot API and webhook send functions. Alert rules live in Lua
// (/scripts/rules.lua, see examples/scripts/rules.lua.example).
#include <thesada_config.h>
#ifdef ENABLE_TELEGRAM
//
//   TelegramModule::send(message)          - send to all configured chat_ids
//   TelegramModule::sendTo(chatId, message) - send to a specific chat_id
// send() also publishes to EventBus("alert") for the MQTT relay.
// SPDX-License-Identifier: GPL-3.0-only
#include "TelegramModule.h"
#include <alert_payload_policy.h>
#include <Config.h>
#include <Secret.h>
#include <EventBus.h>
#include <WiFiManager.h>
#include <MQTTClient.h>
#include <Log.h>
#include <Watchdog.h>
#include <boot_wait_policy.h>
#include <http_url_policy.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include "telegram_ca_progmem.h"
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
TelegramModule::Pending TelegramModule::_queue[TelegramModule::QUEUE_LEN];
uint8_t TelegramModule::_head   = 0;
uint8_t TelegramModule::_queued = 0;
WiFiClientSecure TelegramModule::_secClient;
String TelegramModule::_caCert;
String TelegramModule::_webhookCaCert;
WiFiClientSecure TelegramModule::_webhookClient;

// ---------------------------------------------------------------------------

// Mark module as ready and log the bot token and chat count
void TelegramModule::begin() {
  _ready = true;

  JsonObject cfg    = Config::get();
  char tokenBuf[Secret::MAX_LEN];
  const char* token = Secret::resolve("telegram_token", cfg["telegram"]["bot_token"] | "",
                                      tokenBuf, sizeof(tokenBuf));
  JsonVariant chatIds = cfg["telegram"]["chat_ids"];
  int nChats = 0;
  if (chatIds.is<JsonArray>()) nChats = (int)chatIds.as<JsonArray>().size();
  else if (chatIds.is<JsonObject>()) nChats = (int)chatIds.as<JsonObject>().size();
  bool hasToken = strlen(token) > 0;

  // Configure the persistent TLS clients once. Both are never deleted -
  // avoids the heap fragmentation that kills Telegram after 3 alerts.
  //
  // Bot API client: CA-verified. Load the Telegram API root - LittleFS
  // /telegram-ca.crt wins (rotation without a reflash), else the baked
  // PROGMEM Go Daddy Root G2. Verified TLS replaces the old setInsecure()
  // call - that left every request open to a MITM lifting the bot token
  // out of the request URL.
  _caCert = "";
  if (LittleFS.exists("/telegram-ca.crt")) {
    File cf = LittleFS.open("/telegram-ca.crt", "r");
    if (cf) {
      _caCert = cf.readString();
      cf.close();
      Log::info(TAG, "telegram.ca_loaded path=/telegram-ca.crt");
    }
  }
  if (_caCert.isEmpty()) {
    _caCert = String(FPSTR(TELEGRAM_CA_PROGMEM));
    if (!_caCert.isEmpty()) {
      Log::warn(TAG, "telegram.ca_fallback source=progmem");
    }
  }
  if (!_caCert.isEmpty()) {
    _secClient.setCACert(_caCert.c_str());
  } else {
    // No CA at all: leave the client unconfigured so the handshake
    // fails closed. Refusing to send beats leaking the token insecurely.
    Log::error(TAG, "telegram.ca_missing sends=fail_closed");
  }
  _secClient.setConnectionTimeout(TELEGRAM_IO_MS);  // ms: TCP connect and each write
  _secClient.setHandshakeTimeout(TELEGRAM_HANDSHAKE_S);

  // Webhook client: /webhook-ca.crt on LittleFS enables verified TLS
  // (upload the endpoint's root, self-signed certs included - same
  // override pattern as /telegram-ca.crt). Without it the endpoint is
  // an arbitrary operator-chosen URL we cannot pin, so it stays
  // unverified - documented fallback, not an oversight.
  _webhookCaCert = "";
  bool webhookCaFileExists = LittleFS.exists("/webhook-ca.crt");
  if (webhookCaFileExists) {
    File wf = LittleFS.open("/webhook-ca.crt", "r");
    if (wf) {
      _webhookCaCert = wf.readString();
      wf.close();
    }
  }
  if (!_webhookCaCert.isEmpty()) {
    _webhookClient.setCACert(_webhookCaCert.c_str());
    Log::info(TAG, "telegram.webhook_ca_loaded path=/webhook-ca.crt tls=verified");
  } else {
    _webhookClient.setInsecure();
    if (webhookCaFileExists) {
      // The operator uploaded a CA expecting verified TLS; a quiet insecure
      // fallback here would misreport the security posture, so shout.
      Log::kvfw(TAG, "telegram.webhook_ca_invalid reason=unreadable tls=unverified");
    } else {
      Log::warn(TAG, "telegram.webhook_ca_missing tls=unverified");
    }
  }
  _webhookClient.setConnectionTimeout(TELEGRAM_IO_MS);
  _webhookClient.setHandshakeTimeout(TELEGRAM_HANDSHAKE_S);

  Log::kvf(TAG, "telegram.ready token=%s chats=%d", hasToken ? "yes" : "no", nChats);

  // Register Lua bindings (Telegram.send, Telegram.broadcast)
  #ifdef ENABLE_SCRIPTENGINE
  ScriptEngine::addBindings([](lua_State* L) {
    static const luaL_Reg telegramLib[] = {
      {"send", [](lua_State* L) -> int {
        const char* chatId = luaL_checkstring(L, 1);
        const char* msg = luaL_checkstring(L, 2);
        const uint32_t t0 = millis();
        bool ok = TelegramModule::sendTo(chatId, msg);
        ScriptEngine::creditNative(millis() - t0);
        lua_pushboolean(L, ok);
        return 1;
      }},
      {"broadcast", [](lua_State* L) -> int {
        const char* msg = luaL_checkstring(L, 1);
        const char* code = luaL_optstring(L, 2, "telegram");
        const uint32_t t0 = millis();
        bool ok = TelegramModule::send(msg, code);
        ScriptEngine::creditNative(millis() - t0);
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

// One queued send per pass, at the loop's own shallow stack.
void TelegramModule::loop() {
  if (_queued == 0) return;
  Pending& p = _queue[_head];
  if (p.chat[0]) _deliverTo(p.chat, p.msg);
  else _deliverAll(p.msg);
  _head = (_head + 1) % QUEUE_LEN;
  _queued--;
}

// ---------------------------------------------------------------------------

// Core 3 DNS is uncapped: resolve first and feed, so connect() only hits the cache.
static bool resolveFed(const char* host) {
  IPAddress ip;
  const bool ok = WiFi.hostByName(host, ip) == 1;
  Watchdog::feed();
  return ok;
}

// One POST, status line only. HTTPClient bounds inactivity, not the total, so a
// slow-drip reply could outlast the TWDT; this reads under a hard deadline.
static int boundedPost(Client& c, const char* host, uint16_t port, bool tls, const char* path,
                       const String& body) {
  if (!c.connect(host, port)) { c.stop(); return -1; }
  String req = String("POST ") + httpTargetLead(path) + path + " HTTP/1.1\r\nHost: " + host;
  if (port != (tls ? 443 : 80)) { req += ':'; req += String(port); }
  req += "\r\nContent-Type: application/json\r\nContent-Length: " + String(body.length()) +
         "\r\nConnection: close\r\n\r\n";
  c.print(req);
  c.print(body);
  char line[48];
  size_t n = 0;
  const uint32_t t0 = millis();
  while (millis() - t0 < TELEGRAM_HTTP_TIMEOUT_MS) {
    const int ch = c.read();
    if (ch < 0) {
      if (!c.connected()) break;
      delay(10);
      continue;
    }
    if (ch == '\n') { line[n] = '\0'; c.stop(); return httpStatusCode(line); }
    if (ch != '\r' && n < sizeof(line) - 1) line[n++] = (char)ch;
  }
  c.stop();
  return -2;
}

// Send a message to a specific Telegram chat ID via the Bot API.
// Skips the send if free heap is below TELEGRAM_HEAP_FLOOR_BYTES so the
// TLS handshake here cannot starve the MQTT client of buffer space.
// MQTT has hard priority because it carries OTA.
bool TelegramModule::_deliverTo(const char* chatId, const char* message) {
  if (!chatId || strlen(chatId) == 0 || !message || strlen(message) == 0) return false;
  if (!WiFiManager::connected()) { Log::warn(TAG, "telegram.send_skipped reason=wifi_down"); return false; }

  if (TELEGRAM_HEAP_FLOOR_BYTES > 0) {
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < TELEGRAM_HEAP_FLOOR_BYTES) {
      Log::kvfw(TAG, "telegram.send_skipped reason=heap_low free_b=%lu floor_b=%d priority=mqtt",
                (unsigned long)freeHeap, (int)TELEGRAM_HEAP_FLOOR_BYTES);
      return false;
    }
  }

  JsonObject cfg    = Config::get();
  char tokenBuf[Secret::MAX_LEN];
  const char* token = Secret::resolve("telegram_token", cfg["telegram"]["bot_token"] | "",
                                      tokenBuf, sizeof(tokenBuf));
  if (strlen(token) == 0) { Log::warn(TAG, "telegram.send_skipped reason=no_bot_token"); return false; }

  char path[112];
  snprintf(path, sizeof(path), "/bot%s/sendMessage", token);

  JsonDocument tdoc;
  tdoc["chat_id"] = chatId;
  tdoc["text"]    = message;
  String body;
  serializeJson(tdoc, body);

  // Reuse the persistent TLS client. stop() closes the TCP socket but
  // keeps the mbedtls context allocated - no new heap allocation needed.
  _secClient.stop();

  if (!resolveFed("api.telegram.org")) { Log::warn(TAG, "telegram.send_skipped reason=dns"); return false; }
  int code = boundedPost(_secClient, "api.telegram.org", 443, true, path, body);
  Watchdog::feed();

  if (code == 200) {
    Log::kvf(TAG, "telegram.sent chat_id=%s", chatId);
    return true;
  } else {
    Log::kvfw(TAG, "telegram.send_failed status=%d chat_id=%s", code, chatId);
    return false;
  }
}

// ---------------------------------------------------------------------------

// Broadcast to every chat, the webhook and EventBus("alert"). Only the alert
// event carries the [heap=N] breadcrumb, chats get the plain message.
bool TelegramModule::send(const char* message, const char* code) {
  if (!_ready) { Log::warn(TAG, "telegram.send_skipped reason=not_ready"); return false; }
  if (!code || !*code) code = "telegram";
  // A caller's code past 32 bytes would leave no room for the message.
  char codeBuf[33];
  const size_t codeLen = alertUtf8Cut(code, sizeof(codeBuf) - 1);
  memcpy(codeBuf, code, codeLen);
  codeBuf[codeLen] = '\0';
  alertScrubControls(codeBuf);
  code = codeBuf;

  char body[256];
  strncpy(body, message ? message : "", sizeof(body) - 1);
  body[sizeof(body) - 1] = '\0';
  alertScrubControls(body);
  char crumb[24];
  snprintf(crumb, sizeof(crumb), " [heap=%lu]", (unsigned long)MQTTClient::currentFreeHeap());

  // The app's alert contract in the 256 B MQTT alert slot, breadcrumb kept (alert_payload_policy.h).
  JsonDocument doc;
  doc["severity"] = "warn";
  doc["code"]     = code;
  doc["message"]  = String(body).substring(0, alertMessageFit("warn", code, body, 256 - strlen(crumb))) + crumb;
  EventBus::publish("alert", doc.as<JsonObject>());
  return _enqueue("", message);
}

// Every configured chat, then the optional webhook. Runs from loop() only.
void TelegramModule::_deliverAll(const char* message) {
  // Send to all configured chat_ids (supports both array and object format).
  JsonObject cfg    = Config::get();
  JsonVariant ids   = cfg["telegram"]["chat_ids"];
  // A chat id is a string or an integer (group ids are negative numbers).
  auto sendToId = [message](JsonVariant v) {
    char num[24];
    const char* chatId = v.as<const char*>();
    // A 13-digit id is held as a double here, as<long long>() clamps it to 32 bits.
    if (!chatId && v.is<double>()) {
      const size_t n = serializeJson(v, num, sizeof(num));
      if (n > 0 && n < sizeof(num)) chatId = num;
    }
    if (chatId && *chatId) _deliverTo(chatId, message);
    else Log::warn(TAG, "telegram.send_skipped reason=bad_chat_id");
  };
  if (ids.is<JsonArray>()) {
    for (JsonVariant v : ids.as<JsonArray>()) sendToId(v);
  } else if (ids.is<JsonObject>()) {
    for (JsonPair kv : ids.as<JsonObject>()) sendToId(kv.value());
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

      char host[96];
      const char* wpath;
      uint16_t port;
      bool isHttps = false;
      if (!httpUrlSplit(url, host, sizeof(host), &wpath, &port, &isHttps)) {
        Log::warn(TAG, "telegram.webhook_post_failed reason=bad_url");
      } else if (!resolveFed(host)) {
        Log::warn(TAG, "telegram.webhook_post_failed reason=dns");
      } else {
        WiFiClient pc;
        pc.setConnectionTimeout(TELEGRAM_IO_MS);
        if (isHttps) _webhookClient.stop();
        int code = isHttps ? boundedPost(_webhookClient, host, port, true, wpath, postBody)
                           : boundedPost(pc, host, port, false, wpath, postBody);
        Watchdog::feed();
        if (code > 0) {
          Log::kvf(TAG, "telegram.webhook_response status=%d", code);
        } else {
          Log::warn(TAG, "telegram.webhook_post_failed");
        }
      }
    }
  }
}

// Lua and rules callers sit deep in shell, MQTT or timer chains, too deep for a
// TLS handshake on the loop task, so sends queue here and loop() delivers them.
bool TelegramModule::sendTo(const char* chatId, const char* message) {
  if (!chatId || !*chatId) return false;
  return _enqueue(chatId, message);
}

bool TelegramModule::_enqueue(const char* chatId, const char* message) {
  if (!message || !*message) return false;
  if (strlen(chatId) >= sizeof(_queue[0].chat)) {
    Log::warn(TAG, "telegram.send_skipped reason=bad_chat_id");
    return false;
  }
  if (_queued >= QUEUE_LEN) {
    Log::warn(TAG, "telegram.send_skipped reason=queue_full");
    return false;
  }
  Pending& p = _queue[(_head + _queued) % QUEUE_LEN];
  strcpy(p.chat, chatId);
  strncpy(p.msg, message, sizeof(p.msg) - 1);
  p.msg[sizeof(p.msg) - 1] = '\0';
  _queued++;
  return true;
}

// Report Telegram module status
void TelegramModule::status(ShellOutput out) {
  JsonObject cfg = Config::get();
  char tokenBuf[Secret::MAX_LEN];
  const char* token = Secret::resolve("telegram_token", cfg["telegram"]["bot_token"] | "",
                                      tokenBuf, sizeof(tokenBuf));
  JsonArray chatIds = cfg["telegram"]["chat_ids"].as<JsonArray>();
  int nChats = chatIds.isNull() ? 0 : (int)chatIds.size();
  char line[96];
  snprintf(line, sizeof(line), "token=%s  %d chat(s)", strlen(token) > 0 ? "yes" : "no", nChats);
  out(line);
}

MODULE_REGISTER(TelegramModule, PRIORITY_SERVICE)  // before ScriptEngine(40) so Lua bindings are registered first

#endif // ENABLE_TELEGRAM
