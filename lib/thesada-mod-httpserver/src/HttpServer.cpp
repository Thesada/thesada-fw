// thesada-fw - HttpServer.cpp
// ESPAsyncWebServer-based HTTP server. Serves a live sensor dashboard and
// JSON API. Config editing and OTA are gated behind HTTP Basic Auth using
// credentials from config.json ("web.user" / "web.password").
// State cache is populated via updateState() called from EventBus subscribers
// so the /api/state response is always fresh from the last sensor read.
// SPDX-License-Identifier: GPL-3.0-only

#include <thesada_config.h>
#ifdef ENABLE_WEBSERVER

#include "HttpServer.h"
#include <Config.h>
#include <EventBus.h>
#include <Log.h>
#include <ScriptEngine.h>
#include <Shell.h>
#include <WiFiManager.h>
#include <MQTTClient.h>
#include <ModuleRegistry.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <Update.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <WiFi.h>

static const char* TAG = "WebServer";

static AsyncWebServer server(80);
static AsyncWebSocket _ws("/ws/serial");

// Cached JSON state document - updated by EventBus subscribers.
static JsonDocument _stateDoc;

// Deferred restart flag - set by /api/restart, acted on in loop().
static bool _restartPending = false;

// Buffer for POST /api/config body (written after auth check).
static String _cfgBodyBuf;
// Buffer for POST /api/file body.
static String _fileBodyBuf;
// Buffer for POST /api/cmd body.
static String _cmdBodyBuf;

// ---------------------------------------------------------------------------
// Auth helpers
// ---------------------------------------------------------------------------

// Rate limiter: 5 failures per source IP → 30 s lockout.
struct _RLEntry { char ip[20]; uint8_t fails; uint32_t until; };
static _RLEntry _rlTable[8] = {};
static uint8_t  _rlCount    = 0;

// Check if an IP is allowed or currently rate-limited
static bool _rlAllow(const String& ip) {
  for (uint8_t i = 0; i < _rlCount; i++) {
    if (strcmp(_rlTable[i].ip, ip.c_str()) != 0) continue;
    return millis() >= _rlTable[i].until;
  }
  return true;
}
// Record a failed auth attempt and lock out after 5 failures
static void _rlFail(const String& ip) {
  for (uint8_t i = 0; i < _rlCount; i++) {
    if (strcmp(_rlTable[i].ip, ip.c_str()) != 0) continue;
    if (++_rlTable[i].fails >= 5) _rlTable[i].until = millis() + 30000UL;
    return;
  }
  if (_rlCount < 8) {
    strncpy(_rlTable[_rlCount].ip, ip.c_str(), sizeof(_rlTable[0].ip) - 1);
    _rlTable[_rlCount].ip[sizeof(_rlTable[0].ip) - 1] = '\0';
    _rlTable[_rlCount].fails = 1;
    _rlTable[_rlCount].until = 0;
    _rlCount++;
  }
}
// Clear the failure counter for an IP after a successful auth
static void _rlReset(const String& ip) {
  for (uint8_t i = 0; i < _rlCount; i++) {
    if (strcmp(_rlTable[i].ip, ip.c_str()) != 0) continue;
    _rlTable[i].fails = 0; _rlTable[i].until = 0;
    return;
  }
}

// ---------------------------------------------------------------------------
// Bearer token auth: POST /api/login with Basic Auth -> returns a token.
// Admin endpoints accept either Basic Auth or Bearer token.
// ---------------------------------------------------------------------------
static constexpr uint8_t  MAX_TOKENS = 4;
static constexpr uint32_t TOKEN_TTL_MS = 3600000UL;  // 1 hour
static constexpr uint8_t  TOKEN_LEN = 16;             // 16 bytes = 32 hex chars

struct _TokenEntry { char token[33]; uint32_t expiry; };
static _TokenEntry _tokens[MAX_TOKENS] = {};

// Generate a random hex token
static void _genToken(char* out) {
  for (uint8_t i = 0; i < TOKEN_LEN; i++) {
    uint8_t b = (uint8_t)esp_random();
    sprintf(out + i * 2, "%02x", b);
  }
  out[TOKEN_LEN * 2] = '\0';
}

// Create a new token and return it (evicts oldest if full)
static const char* _createToken() {
  uint32_t now = millis();
  uint8_t slot = 0;
  uint32_t oldest = UINT32_MAX;
  for (uint8_t i = 0; i < MAX_TOKENS; i++) {
    if (_tokens[i].expiry == 0 || now >= _tokens[i].expiry) { slot = i; break; }
    if (_tokens[i].expiry < oldest) { oldest = _tokens[i].expiry; slot = i; }
  }
  _genToken(_tokens[slot].token);
  _tokens[slot].expiry = now + TOKEN_TTL_MS;
  return _tokens[slot].token;
}

// Validate a Bearer token
static bool _validateToken(const char* token) {
  if (!token || strlen(token) == 0) return false;
  uint32_t now = millis();
  for (uint8_t i = 0; i < MAX_TOKENS; i++) {
    if (_tokens[i].expiry > 0 && now < _tokens[i].expiry &&
        strcmp(_tokens[i].token, token) == 0) {
      return true;
    }
  }
  return false;
}

// Check auth: Basic Auth OR Bearer token. Returns true if authorized.
static bool _checkAuth(AsyncWebServerRequest* req, const String& webUser, const String& webPass) {
  // Check Bearer token first
  if (req->hasHeader("Authorization")) {
    String authHeader = req->header("Authorization");
    if (authHeader.startsWith("Bearer ")) {
      String token = authHeader.substring(7);
      if (_validateToken(token.c_str())) return true;
    }
  }
  // Fall back to Basic Auth
  return req->authenticate(webUser.c_str(), webPass.c_str());
}

// IP-based WS pre-auth: GET /api/ws/token (auth-gated) marks the caller's IP as
// authorized for 30 s. WS_EVT_CONNECT checks the IP - no query-param parsing needed.
struct _WsAuth { char ip[20]; uint32_t expiry; };
static _WsAuth _wsAuth[4] = {};

// Grant a 30-second WebSocket authorization window for the given IP
static void _grantWsAuth(const String& ip) {
  uint32_t now = millis();
  uint8_t  slot = 0;
  for (uint8_t i = 0; i < 4; i++) {
    if (_wsAuth[i].expiry == 0 || now >= _wsAuth[i].expiry) { slot = i; break; }
  }
  strncpy(_wsAuth[slot].ip, ip.c_str(), sizeof(_wsAuth[0].ip) - 1);
  _wsAuth[slot].ip[sizeof(_wsAuth[0].ip) - 1] = '\0';
  _wsAuth[slot].expiry = now + 30000UL;
}

// Consume a one-time WebSocket auth token for the given IP
static bool _consumeWsAuth(const String& ip) {
  uint32_t now = millis();
  for (uint8_t i = 0; i < 4; i++) {
    if (now < _wsAuth[i].expiry && strcmp(_wsAuth[i].ip, ip.c_str()) == 0) {
      _wsAuth[i].expiry = 0;  // one-time use
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Embedded dashboard HTML (served from PROGMEM - see dashboard.html.h).
// ---------------------------------------------------------------------------
#include "dashboard.html.h"

// ---------------------------------------------------------------------------

// Start the HTTP server, register routes, and hook up log broadcasting
void HttpServer::begin() {
  if (!LittleFS.begin()) {
    Log::warn(TAG, "LittleFS not mounted - config editor will be read-only");
  }

  // Relay log lines to WebSocket terminal clients.
  Log::setRemoteHandler(&HttpServer::broadcastLog);

  subscribeToEvents();
  setupRoutes();

  server.begin();
  Log::info(TAG, "HTTP server started on port 80");
}

// ---------------------------------------------------------------------------

// Send a log line to all connected WebSocket terminal clients
void HttpServer::broadcastLog(const char* line) {
  _ws.textAll(line);
}

// ---------------------------------------------------------------------------

// Log the current cached sensor state as JSON
void HttpServer::printState() {
  String out;
  serializeJson(_stateDoc, out);
  Log::info("Sensors", out.c_str());
}

// ---------------------------------------------------------------------------

// Clean up WS clients and handle deferred restart requests
void HttpServer::loop() {
  _ws.cleanupClients();  // required by ESPAsyncWebServer 3.x to drain the WS send queue
  if (_restartPending) {
    Log::info(TAG, "Restarting as requested...");
    delay(500);
    ESP.restart();
  }
}

// ---------------------------------------------------------------------------

// Merge incoming event data into the cached state document under the given key
void HttpServer::updateState(const char* key, JsonObject data) {
  // Merge incoming event data into the top-level state document.
  JsonObject dest = _stateDoc[key].to<JsonObject>();
  for (JsonPair kv : data) {
    dest[kv.key()] = kv.value();
  }
}

// ---------------------------------------------------------------------------

// Subscribe to sensor EventBus events to keep the state cache current
void HttpServer::subscribeToEvents() {
  // Subscribe to all sensor EventBus events so the state cache stays current.
  EventBus::subscribe("temperature", [](JsonObject data) {
    HttpServer::updateState("temperature", data);
  });
  EventBus::subscribe("current", [](JsonObject data) {
    HttpServer::updateState("current", data);
  });
  EventBus::subscribe("battery", [](JsonObject data) {
    HttpServer::updateState("battery", data);
  });
}

// ---------------------------------------------------------------------------

// Register all HTTP routes, WebSocket handler, OTA endpoint, and captive portal
void HttpServer::setupRoutes() {
  JsonObject cfg = Config::get();
  String webUser = cfg["web"]["user"]     | "admin";
  String webPass = cfg["web"]["password"] | "changeme";

  if (webPass == "changeme") {
    Log::warn(TAG, "Default password in use - change web.password in config.json");
  }

  // ── Dashboard HTML - public (sensor data is read-only) ────────────────────
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    AsyncWebServerResponse* resp = req->beginResponse_P(200, "text/html", (const uint8_t*)DASHBOARD_HTML, strlen_P(DASHBOARD_HTML));
    req->send(resp);
  });

  // ── GET /api/auth/check - credential verification (plain 200 or 401) ─────
  // Must NOT call requestAuthentication() here - that would send WWW-Authenticate
  // which causes the browser to silently retry with cached credentials, bypassing
  // the JS modal check entirely.
  server.on("/api/auth/check", HTTP_GET, [webUser, webPass](AsyncWebServerRequest* req) {
    String ip = req->client()->remoteIP().toString();
    if (!_rlAllow(ip)) {
      req->send(429, "application/json", "{\"ok\":false,\"error\":\"Too many attempts - wait 30s\"}");
      return;
    }
    if (!req->authenticate(webUser.c_str(), webPass.c_str())) {
      _rlFail(ip);
      req->send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}");
      return;
    }
    _rlReset(ip);
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // ── POST /api/login - exchange Basic Auth for a Bearer token ────────────────
  server.on("/api/login", HTTP_POST, [webUser, webPass](AsyncWebServerRequest* req) {
    String ip = req->client()->remoteIP().toString();
    if (!_rlAllow(ip)) {
      req->send(429, "application/json", "{\"ok\":false,\"error\":\"Too many attempts - wait 30s\"}");
      return;
    }
    if (!req->authenticate(webUser.c_str(), webPass.c_str())) {
      _rlFail(ip);
      req->send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}");
      return;
    }
    _rlReset(ip);
    const char* token = _createToken();
    char body[128];
    snprintf(body, sizeof(body),
      "{\"ok\":true,\"token\":\"%s\",\"expires_in\":%lu}",
      token, (unsigned long)(TOKEN_TTL_MS / 1000));
    req->send(200, "application/json", body);
    Log::info(TAG, "Token issued");
  });

  // ── GET /api/ws/token - issue a short-lived WS token (auth required) ────────
  // Clients call this before opening /ws/serial, then pass ?token=<hex>.
  server.on("/api/ws/token", HTTP_GET, [webUser, webPass](AsyncWebServerRequest* req) {
    if (!_checkAuth(req, webUser, webPass)) {
      req->send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}");
      return;
    }
    _grantWsAuth(req->client()->remoteIP().toString());
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // ── GET /api/info - firmware version + build info (no auth) ──────────────
  server.on("/api/info", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonObject cfg = Config::get();
    const char* devName = cfg["device"]["friendly_name"] | cfg["device"]["name"] | "thesada-node";
    bool sdAvail = false;
#ifdef ENABLE_SD
    sdAvail = SD_MMC.cardType() != CARD_NONE;
#endif
    char buf[160];
    snprintf(buf, sizeof(buf),
      "{\"version\":\"%s\",\"build\":\"%s %s\",\"device\":\"%s\",\"sd\":%s}",
      FIRMWARE_VERSION, __DATE__, __TIME__, devName, sdAvail ? "true" : "false");
    req->send(200, "application/json", buf);
  });

  // ── GET /api/state - current sensor readings + MQTT status (public) ────────
  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
    // Copy sensor state and inject _mqtt metadata so the dashboard can show
    // connection status and last publish time without a separate request.
    JsonDocument resp;
    for (JsonPair kv : _stateDoc.as<JsonObject>()) {
      resp[kv.key()] = kv.value();
    }
    JsonObject mqttMeta = resp["_mqtt"].to<JsonObject>();
    mqttMeta["connected"] = MQTTClient::connected();
    time_t lp = MQTTClient::lastPublishTime();
    if (lp > 1700000000UL) {
      char ts[22];
      struct tm* t = gmtime(&lp);
      strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", t);
      mqttMeta["last_publish"] = ts;
    } else {
      mqttMeta["last_publish"] = nullptr;
    }
    String out;
    serializeJson(resp, out);
    req->send(200, "application/json", out);
  });

  // ── GET /api/config - read config.json ────────────────────────────────────
  server.on("/api/config", HTTP_GET, [webUser, webPass](AsyncWebServerRequest* req) {
    if (!_checkAuth(req, webUser, webPass)) { req->send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}"); return; }
    if (!LittleFS.exists("/config.json")) {
      req->send(404, "application/json", "{\"error\":\"config.json not found\"}");
      return;
    }
    req->send(LittleFS, "/config.json", "application/json");
  });

  // ── POST /api/config - write config.json (auth required) ──────────────────
  // Body is buffered in _cfgBodyBuf; file is only written after auth passes
  // in onRequest (which runs after all body chunks have arrived).
  AsyncCallbackWebHandler* cfgHandler = new AsyncCallbackWebHandler();
  cfgHandler->setUri("/api/config");
  cfgHandler->setMethod(HTTP_POST);
  cfgHandler->onBody([](AsyncWebServerRequest* req, uint8_t* data, size_t len,
                        size_t index, size_t total) {
    if (index == 0) { _cfgBodyBuf = ""; _cfgBodyBuf.reserve(total); }
    _cfgBodyBuf.concat((const char*)data, len);
  });
  cfgHandler->onRequest([webUser, webPass](AsyncWebServerRequest* req) {
    if (!_checkAuth(req, webUser, webPass)) {
      _cfgBodyBuf = "";  // discard unauthenticated body
      req->send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}");
      return;
    }
    if (!_cfgBodyBuf.isEmpty()) {
      File f = LittleFS.open("/config.json", "w");
      if (f) { f.print(_cfgBodyBuf); f.close(); }
      _cfgBodyBuf = "";
    }
    req->send(200, "application/json", "{\"ok\":true}");
    _restartPending = true;
  });
  server.addHandler(cfgHandler);

  // ── POST /api/backup - copy config.json to SD (auth required) ────────────
  server.on("/api/backup", HTTP_POST, [webUser, webPass](AsyncWebServerRequest* req) {
    if (!_checkAuth(req, webUser, webPass)) { req->send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}"); return; }
    if (!LittleFS.exists("/config.json")) {
      req->send(200, "application/json", "{\"ok\":false,\"error\":\"config.json not found\"}");
      return;
    }
    File src = LittleFS.open("/config.json", "r");
    if (!src) {
      req->send(200, "application/json", "{\"ok\":false,\"error\":\"read failed\"}");
      return;
    }
    File dst = SD_MMC.open("/config_backup.json", FILE_WRITE);
    if (!dst) {
      src.close();
      req->send(200, "application/json", "{\"ok\":false,\"error\":\"SD write failed - is card mounted?\"}");
      return;
    }
    while (src.available()) dst.write(src.read());
    src.close(); dst.close();
    Log::info(TAG, "Config backed up to SD:/config_backup.json");
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // ── POST /api/restart (auth required) ─────────────────────────────────────
  server.on("/api/restart", HTTP_POST, [webUser, webPass](AsyncWebServerRequest* req) {
    if (!_checkAuth(req, webUser, webPass)) { req->send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}"); return; }
    req->send(200, "application/json", "{\"ok\":true}");
    _restartPending = true;
  });

  // ── POST /api/cmd - run a shell command, return JSON output (auth required)
  // Body: {"cmd": "version"} → {"ok": true, "output": ["thesada-fw v1.x ..."]}
  // All Shell built-ins are available. Output is one JSON string per line.
  AsyncCallbackWebHandler* cmdHandler = new AsyncCallbackWebHandler();
  cmdHandler->setUri("/api/cmd");
  cmdHandler->setMethod(HTTP_POST);
  cmdHandler->onBody([](AsyncWebServerRequest* req, uint8_t* data, size_t len,
                        size_t index, size_t total) {
    if (index == 0) { _cmdBodyBuf = ""; _cmdBodyBuf.reserve(total); }
    _cmdBodyBuf.concat((const char*)data, len);
  });
  cmdHandler->onRequest([webUser, webPass](AsyncWebServerRequest* req) {
    if (!_checkAuth(req, webUser, webPass)) {
      _cmdBodyBuf = "";
      req->send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}");
      return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, _cmdBodyBuf);
    _cmdBodyBuf = "";
    if (err || !doc["cmd"].is<const char*>()) {
      req->send(400, "application/json", "{\"ok\":false,\"error\":\"expected {\\\"cmd\\\":\\\"...\\\"}\"}");
      return;
    }
    String cmdStr = doc["cmd"].as<String>();  // copy before doc goes out of scope

    // Collect all output lines; escape for JSON inline.
    String output = "[";
    bool first = true;
    Shell::execute(cmdStr.c_str(), [&output, &first](const char* line) {
      if (!first) output += ",";
      output += "\"";
      for (const char* p = line; *p; ++p) {
        if      (*p == '"')  output += "\\\"";
        else if (*p == '\\') output += "\\\\";
        else if (*p == '\n') output += "\\n";
        else if (*p == '\r') output += "\\r";
        else                 output += *p;
      }
      output += "\"";
      first = false;
    });
    output += "]";

    String resp = "{\"ok\":true,\"output\":";
    resp += output;
    resp += "}";
    req->send(200, "application/json", resp);
  });
  server.addHandler(cmdHandler);

  // ── POST /ota - firmware binary upload (auth required) ────────────────────
  // Accepts multipart/form-data with field "firmware". Uses ESP-IDF Update.h.
  server.on("/ota", HTTP_POST,
    [webUser, webPass](AsyncWebServerRequest* req) {
      if (!_checkAuth(req, webUser, webPass)) { req->send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}"); return; }
      bool ok = !Update.hasError();
      if (ok) {
        req->send(200, "application/json", "{\"ok\":true}");
        _restartPending = true;
      } else {
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", Update.errorString());
        req->send(200, "application/json", resp);
      }
    },
    [](AsyncWebServerRequest* req, const String& filename,
       size_t index, uint8_t* data, size_t len, bool final) {
      if (index == 0) {
        // Use content length from request if available, otherwise unknown
        size_t totalSize = req->contentLength();
        char msg[64];
        snprintf(msg, sizeof(msg), "OTA upload: %s (%u bytes)", filename.c_str(), (unsigned)totalSize);
        Log::info(TAG, msg);
        if (!Update.begin(totalSize > 0 ? totalSize : UPDATE_SIZE_UNKNOWN)) {
          char err[64];
          snprintf(err, sizeof(err), "Update.begin() failed: %s", Update.errorString());
          Log::error(TAG, err);
        }
      }
      if (Update.isRunning()) {
        if (Update.write(data, len) != len) {
          char err[64];
          snprintf(err, sizeof(err), "Update.write() failed: %s", Update.errorString());
          Log::error(TAG, err);
        }
      }
      if (final) {
        if (Update.end(true)) {
          char msg[48];
          snprintf(msg, sizeof(msg), "OTA complete: %u bytes", (unsigned)(index + len));
          Log::info(TAG, msg);
        } else {
          char err[64];
          snprintf(err, sizeof(err), "OTA failed: %s", Update.errorString());
          Log::error(TAG, err);
        }
      }
    }
  );

  // ── GET /api/files - list files (auth required) ──────────────────────────
  // Query param: source=sd (default) or source=littlefs or source=scripts
  server.on("/api/files", HTTP_GET, [webUser, webPass](AsyncWebServerRequest* req) {
    if (!_checkAuth(req, webUser, webPass)) { req->send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}"); return; }
    String src = req->hasParam("source") ? req->getParam("source")->value() : "sd";
    bool useLFS     = (src == "littlefs");
    bool useScripts = (src == "scripts");
    File root = useScripts ? LittleFS.open("/scripts")
              : useLFS     ? LittleFS.open("/")
                           : SD_MMC.open("/");
    if (!root || !root.isDirectory()) {
      req->send(200, "application/json", (useLFS || useScripts) ? "{\"error\":\"LittleFS not mounted\"}" : "{\"error\":\"SD not mounted\"}");
      return;
    }
    String json = "[";
    bool first = true;
    File f = root.openNextFile();
    while (f) {
      if (!first) json += ",";
      json += "{\"name\":\"";
      json += f.name();
      json += "\",\"size\":";
      json += f.size();
      json += ",\"dir\":";
      json += f.isDirectory() ? "true" : "false";
      json += "}";
      first = false;
      f = root.openNextFile();
    }
    json += "]";
    req->send(200, "application/json", json);
  });

  // ── GET /api/file?path=...&source=sd|littlefs|scripts - read file (auth, max 64 KB)
  server.on("/api/file", HTTP_GET, [webUser, webPass](AsyncWebServerRequest* req) {
    if (!_checkAuth(req, webUser, webPass)) { req->send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}"); return; }
    if (!req->hasParam("path")) { req->send(400, "text/plain", "missing path"); return; }
    String path = req->getParam("path")->value();
    String src = req->hasParam("source") ? req->getParam("source")->value() : "sd";
    bool useLFS     = (src == "littlefs");
    bool useScripts = (src == "scripts");
    if (useScripts) path = "/scripts" + path;
    File f = (useLFS || useScripts) ? LittleFS.open(path.c_str(), FILE_READ) : SD_MMC.open(path.c_str(), FILE_READ);
    if (!f || f.isDirectory()) { req->send(404, "text/plain", "not found"); return; }
    const size_t MAX_READ = 65536;
    String content;
    content.reserve(min((size_t)f.size(), MAX_READ));
    while (f.available() && content.length() < MAX_READ) {
      content += (char)f.read();
    }
    f.close();
    req->send(200, "text/plain", content);
  });

  // ── DELETE /api/file?path=...&source=sd|littlefs|scripts - delete file (auth)
  server.on("/api/file", HTTP_DELETE, [webUser, webPass](AsyncWebServerRequest* req) {
    if (!_checkAuth(req, webUser, webPass)) { req->send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}"); return; }
    if (!req->hasParam("path")) { req->send(400, "application/json", "{\"error\":\"missing path\"}"); return; }
    String path = req->getParam("path")->value();
    String src = req->hasParam("source") ? req->getParam("source")->value() : "sd";
    bool useLFS     = (src == "littlefs");
    bool useScripts = (src == "scripts");
    if (useScripts) path = "/scripts" + path;
    bool ok = (useLFS || useScripts) ? LittleFS.remove(path.c_str()) : SD_MMC.remove(path.c_str());
    req->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"delete failed\"}");
  });

  // ── POST /api/file?path=... - write SD file (auth required) ──────────────
  AsyncCallbackWebHandler* fileHandler = new AsyncCallbackWebHandler();
  fileHandler->setUri("/api/file");
  fileHandler->setMethod(HTTP_POST);
  fileHandler->onBody([](AsyncWebServerRequest* req, uint8_t* data, size_t len,
                         size_t index, size_t total) {
    if (index == 0) {
      _fileBodyBuf = "";
      _fileBodyBuf.reserve(total);
    }
    _fileBodyBuf.concat((const char*)data, len);
  });
  fileHandler->onRequest([webUser, webPass](AsyncWebServerRequest* req) {
    if (!_checkAuth(req, webUser, webPass)) {
      _fileBodyBuf = "";
      req->send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}");
      return;
    }
    if (!req->hasParam("path")) {
      _fileBodyBuf = "";
      req->send(400, "application/json", "{\"error\":\"missing path\"}");
      return;
    }
    String path    = req->getParam("path")->value();
    String src     = req->hasParam("source") ? req->getParam("source")->value() : "sd";
    bool   useLFS     = (src == "littlefs");
    bool   useScripts = (src == "scripts");
    if (useScripts) path = "/scripts" + path;
    File f = (useLFS || useScripts) ? LittleFS.open(path.c_str(), FILE_WRITE)
                                    : SD_MMC.open(path.c_str(), FILE_WRITE);
    if (!f) {
      _fileBodyBuf = "";
      req->send(200, "application/json", (useLFS || useScripts) ? "{\"ok\":false,\"error\":\"LittleFS write failed\"}"
                                                                 : "{\"ok\":false,\"error\":\"SD write failed\"}");
      return;
    }
    if (_fileBodyBuf.length() == 0) {
      f.close();
      _fileBodyBuf = "";
      req->send(200, "application/json", "{\"ok\":false,\"error\":\"Empty body - try again\"}");
      return;
    }
    f.print(_fileBodyBuf);
    size_t written = f.size();
    f.close();
    _fileBodyBuf = "";
    char rmsg[64];
    snprintf(rmsg, sizeof(rmsg), "{\"ok\":true,\"bytes\":%d}", (int)written);
    req->send(200, "application/json", rmsg);
  });
  server.addHandler(fileHandler);

  // ── WebSocket /ws/serial - bidirectional serial terminal ──────────────────
  // All commands are handled by the Shell core class.
  _ws.onEvent([](AsyncWebSocket* srv, AsyncWebSocketClient* client,
                 AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
      // Use the AsyncWebSocketClient lambda param for remoteIP(), not
      // req->client(). On WS_EVT_CONNECT the underlying lwIP pcb has
      // already been transferred from the HTTP request's AsyncClient to
      // the new AsyncWebSocketClient via _switchClient, so
      // req->client()->_pcb is NULL and the old API null-derefs in
      // AsyncClient::getRemoteAddress().
      String ip = client->remoteIP().toString();
      if (!_consumeWsAuth(ip)) {
        Log::warn(TAG, "WS: rejected - not pre-authorized");
        client->close();
        return;
      }
      // Replay last N log lines as a single concatenated WS message.
      uint8_t count = Log::ringCount();
      if (count > 0) {
        String replay;
        replay.reserve(count * 120);
        for (uint8_t i = 0; i < count; i++) {
          const char* line = Log::ringLine(i);
          if (line) {
            if (replay.length() > 0) replay += '\n';
            replay += line;
          }
        }
        if (replay.length() > 0) {
          client->text(replay);
        }
      }
    } else if (type == WS_EVT_DATA) {
      // Only accept final, unfragmented text frames. On WS_EVT_DATA the
      // arg pointer is an AwsFrameInfo* describing the current frame.
      // Without this check we'd pass binary opcodes, fragment headers
      // or partial text chunks straight to Shell::execute as garbage.
      AwsFrameInfo* info = (AwsFrameInfo*)arg;
      if (!info || !info->final || info->index != 0 || info->len != len ||
          info->opcode != WS_TEXT) {
        return;
      }
      String cmd = String((char*)data, len);
      cmd.trim();
      if (cmd.length() > 0) {
        Shell::execute(cmd.c_str(), [client](const char* line) {
          client->text(line);
        });
      }
    }
  });
  server.addHandler(&_ws);

  // ── Captive portal catch-all (AP mode only) ────────────────────────────────
  // Redirect any request to a non-local host to the AP IP so phones/laptops
  // auto-open the dashboard when connecting to the setup AP.
  server.onNotFound([](AsyncWebServerRequest* req) {
    if (WiFiManager::isAPActive()) {
      req->redirect("http://" + WiFi.softAPIP().toString() + "/");
    } else {
      req->send(404, "text/plain", "Not found");
    }
  });

  Log::info(TAG, "Routes registered");
}

// Module wrapper for self-registration
class HttpServerModule : public Module {
public:
  // Delegate to static HttpServer::begin
  void begin() override { HttpServer::begin(); }
  // Delegate to static HttpServer::loop
  void loop() override { HttpServer::loop(); }
  // Return module name
  const char* name() override { return "HttpServer"; }
};

MODULE_REGISTER(HttpServerModule, PRIORITY_SERVICE)

#endif // ENABLE_WEBSERVER
