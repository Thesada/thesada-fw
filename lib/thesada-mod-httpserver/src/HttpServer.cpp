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
#include <Secret.h>
#include <web_auth_policy.h>
#include <login_rate_policy.h>
#include <ttl_policy.h>
#include <ws_grant_policy.h>
#include <http_body_policy.h>
#include <EventBus.h>
#include <Log.h>
#include <ScriptEngine.h>
#include <Shell.h>
#include <WiFiManager.h>
#include <MQTTClient.h>
#include <OTAUpdate.h>
#include <SetupPortal.h>
#include <ModuleRegistry.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <Update.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <mbedtls/platform_util.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <enroll_policy.h>
#include <config_write_policy.h>
#include <web_snapshot_policy.h>
#include <shell_mode_policy.h>
#include <string.h>
#include <atomic>

static const char* TAG = "WebServer";

static AsyncWebServer server(80);
static AsyncWebSocket _ws("/ws/serial");

// Cached JSON state document - updated by EventBus subscribers.
static JsonDocument _stateDoc;
// /api/state reads this copy, never _stateDoc: the loop rebuilds the doc while async_tcp serves.
// The loop swaps in a fresh heap copy and frees the old one outside the lock.
static constexpr size_t STATE_JSON_MAX = 16384;
static char*        _stateJson = nullptr;
static size_t       _stateJsonLen = 0;
static portMUX_TYPE _stateMux = portMUX_INITIALIZER_UNLOCKED;

// Deferred restart flag - set by /api/restart, acted on in loop().
static std::atomic<bool> _restartPending{false};

// Set once server.begin() has actually bound. See netifUp() below.
static bool _serverStarted = false;

// ESPAsyncWebServer binds an lwIP socket, and lwIP only exists once WiFi has
// started in some mode. With wifi.enabled=false nothing ever starts it, so
// server.begin() asserts in tcpip_api_call and panics the boot.
static bool netifUp() {
  return WiFi.getMode() != WIFI_MODE_NULL;
}

// ---------------------------------------------------------------------------
// Auth helpers
// ---------------------------------------------------------------------------

// Clock for every auth deadline here (lockout, tokens, WS grants): 64-bit
// uptime ms, because a millis() deadline goes stale at the 49.7-day wrap.
static uint64_t _uptimeMs() { return (uint64_t)esp_timer_get_time() / 1000; }

// Login lockout per source IP: 5 counted guesses -> 30 s (login_rate_policy.h).
static LoginRateTable<> _rl;

// False while ip is locked out. in: source ip. out: true to serve the attempt.
static bool _rlAllow(const String& ip) { return _rl.allow(ip.c_str(), _uptimeMs()); }
// Count one wrong guess from ip. in: source ip.
static void _rlFail(const String& ip) { _rl.fail(ip.c_str(), _uptimeMs()); }
// Forget ip after a successful auth. in: source ip.
static void _rlReset(const String& ip) { _rl.reset(ip.c_str()); }

// Reject path traversal attempts. Returns true if path is safe.
// Delegates to Shell::pathSafe so HTTP, Shell (serial/WS), and MQTT CLI
// share one policy - any tightening lands in one place. See Shell.h.
// in: path string. out: true if safe, false if traversal detected.
static bool _pathSafe(const String& path) {
  return Shell::pathSafe(path.c_str());
}

// ---------------------------------------------------------------------------
// Bearer token auth: POST /api/login with Basic Auth -> returns a token.
// Admin endpoints accept either Basic Auth or Bearer token.
// ---------------------------------------------------------------------------
static constexpr uint8_t  MAX_TOKENS = 4;
static constexpr uint32_t TOKEN_TTL_MS = 3600000UL;  // 1 hour
static constexpr uint8_t  TOKEN_LEN = 16;             // 16 bytes = 32 hex chars

struct _TokenEntry { char token[33]; uint64_t expiry; };
static _TokenEntry _tokens[MAX_TOKENS] = {};

// Generate a random hex token
static void _genToken(char* out) {
  for (uint8_t i = 0; i < TOKEN_LEN; i++) {
    uint8_t b = (uint8_t)esp_random();
    snprintf(out + i * 2, 3, "%02x", b);
  }
  out[TOKEN_LEN * 2] = '\0';
}

// Create a new token and return it (evicts the one closest to expiry if full)
static const char* _createToken() {
  const uint64_t now = _uptimeMs();
  uint8_t slot = 0;
  for (uint8_t i = 0; i < MAX_TOKENS; i++) {
    if (!ttlActive(now, _tokens[i].expiry)) { slot = i; break; }
    if (_tokens[i].expiry < _tokens[slot].expiry) slot = i;
  }
  _genToken(_tokens[slot].token);
  _tokens[slot].expiry = now + TOKEN_TTL_MS;
  return _tokens[slot].token;
}

// Validate a Bearer token. Compares in constant time and walks every
// slot regardless of an early match, so neither the per-byte compare
// nor the slot loop leaks token correctness via response timing.
static bool _validateToken(const char* token) {
  if (!token) return false;
  size_t tlen = strlen(token);
  if (tlen != TOKEN_LEN * 2) return false;
  const uint64_t now = _uptimeMs();
  bool found = false;
  for (uint8_t i = 0; i < MAX_TOKENS; i++) {
    bool active = ttlActive(now, _tokens[i].expiry);
    bool match  = constTimeEq(_tokens[i].token, token, tlen);
    if (active && match) found = true;
  }
  return found;
}

// One warn per minute per call site, so a probe flood cannot fill the ring.
static bool _warnDue(uint32_t& lastMs) {
  const uint32_t now = millis();
  if (lastMs != 0 && now - lastMs <= 60000UL) return false;
  lastMs = now;
  return true;
}

// What the async_tcp handlers read from config, copied on the main loop. A doc
// rebuild there frees strings, so no handler may walk Config itself.
static_assert(WEB_SNAP_PASS_CAP == Secret::MAX_LEN, "snapshot holds what Secret::resolve copies");
static WebSnap      _webSnap = {};
static portMUX_TYPE _webSnapMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t     _webSnapRev = 0;
static bool         _webSnapSet = false;

// Main loop only, run from Config::onChange and loop(). in: none. out: none.
static void _webSnapRefresh() {
  JsonObject cfg = Config::get();
  WebSnap s;
  webSnapFill(s, cfg["web"]["user"] | "admin", cfg["web"]["password"] | "changeme",
              cfg["device"]["friendly_name"] | cfg["device"]["name"] | WEB_SNAP_NAME_DEFAULT);
  taskENTER_CRITICAL(&_webSnapMux);
  _webSnap = s;
  taskEXIT_CRITICAL(&_webSnapMux);
  mbedtls_platform_zeroize(s.passCfg, sizeof(s.passCfg));
  _webSnapRev = Config::revision();
  _webSnapSet = true;
  if (!s.userFits || !s.passFits) {
    Log::kvfw(TAG, "web.credential_too_long user_fits=%d password_fallback_fits=%d",
              s.userFits ? 1 : 0, s.passFits ? 1 : 0);
  }
}

// Any task. in: destination. out: a consistent copy, zeroize passCfg after use.
static void _webSnapRead(WebSnap& out) {
  taskENTER_CRITICAL(&_webSnapMux);
  out = _webSnap;
  taskEXIT_CRITICAL(&_webSnapMux);
}

// Snapshot user plus the effective password, resolved per call so secret.set /
// secret.clear web.password apply at once. in: buffers. out: credential state.
static WebCredState _webCred(char* user, size_t userCap, char* pass, size_t passCap) {
  WebSnap snap;
  _webSnapRead(snap);
  const char* p = Secret::resolve("web_password", snap.passCfg, pass, passCap);
  WebCredState st = webCredState(snap, p);
  mbedtls_platform_zeroize(snap.passCfg, sizeof(snap.passCfg));
  if (user) webSnapCopy(user, userCap, snap.user);
  return st;
}

// in: none. out: why nothing can authenticate, WEB_CRED_OK when something can.
static WebCredState _webCredLock() {
  char passBuf[Secret::MAX_LEN];
  WebCredState st = _webCred(nullptr, 0, passBuf, sizeof(passBuf));
  mbedtls_platform_zeroize(passBuf, sizeof(passBuf));
  return st;
}

// 403 naming the lock, for the login routes. in: request, state. out: true if sent.
static bool _sendCredLocked(AsyncWebServerRequest* req, WebCredState st) {
  if (st == WEB_CRED_DEFAULT) {
    req->send(403, "application/json",
              "{\"ok\":false,\"error\":\"web.password is default/empty - admin locked. "
              "Set it via 'secret.set web.password' on the serial console\"}");
    return true;
  }
  if (st == WEB_CRED_TOO_LONG) {
    req->send(403, "application/json",
              "{\"ok\":false,\"error\":\"web.user (max 63) or the config web.password "
              "(max 159) is too long - admin locked. Shorten it, or set the password "
              "via 'secret.set web.password'\"}");
    return true;
  }
  return false;
}

// Check auth: Basic Auth OR Bearer token. Returns true if authorized.
// Credentials are resolved per request so secret.set / secret.clear
// web.password take effect without a restart.
// While web.password is default/empty, nothing authenticates - not even a
// Bearer token (see web_auth_policy.h). The dashboard and other public
// routes stay up; only the authenticated surface is refused.
// hasSideEffect marks a route whose safe-looking method still changes state,
// so the cross-site Basic rule applies to it (GET /api/ws/token).
// guessed, when given, reports whether a credential was actually offered and
// wrong - the login rate limiter must count only those.
static bool _checkAuth(AsyncWebServerRequest* req, bool hasSideEffect = false,
                       bool* guessed = nullptr) {
  if (guessed) *guessed = false;
  char webUser[WEB_SNAP_USER_CAP];
  char passBuf[Secret::MAX_LEN];
  const WebCredState cred = _webCred(webUser, sizeof(webUser), passBuf, sizeof(passBuf));
  const char* webPass = passBuf;

  // A default or a too-long credential vetoes everything, Bearer included.
  const bool passIsDefault = cred != WEB_CRED_OK;
  if (cred == WEB_CRED_DEFAULT) {
    static uint32_t lastWarnMs = 0;
    if (_warnDue(lastWarnMs)) {
      Log::warn(TAG, "web.admin_refused reason=default_password "
                     "hint=\"set via secret.set web.password or config.json\"");
    }
  } else if (cred == WEB_CRED_TOO_LONG) {
    static uint32_t lastLongWarnMs = 0;
    if (_warnDue(lastLongWarnMs)) {
      Log::warn(TAG, "web.admin_refused reason=credential_too_long "
                     "hint=\"web.user max 63, config web.password max 159\"");
    }
  }

  bool bearerValid   = false;
  bool bearerOffered = false;
  bool basicOffered  = false;
  if (req->hasHeader("Authorization")) {
    const String& authHeader = req->header("Authorization");
    if (authHeader.startsWith("Bearer ")) {
      bearerOffered = true;
      bearerValid = _validateToken(authHeader.substring(7).c_str());
    } else if (authHeader.startsWith("Basic ")) {
      basicOffered = true;
    }
  }
  // Cached Basic creds ride a cross-site request; Bearer cannot (see
  // web_auth_policy.h). curl and pre-Sec-Fetch browsers send no header.
  const bool stateChanging = hasSideEffect || webAuthMethodChangesState(req->methodToString());
  const bool basicAllowed = webAuthBasicAllowed(
      stateChanging,
      req->hasHeader("Sec-Fetch-Site") ? req->header("Sec-Fetch-Site").c_str() : nullptr);
  if (!basicAllowed && basicOffered && !bearerValid) {
    static uint32_t lastCsrfWarnMs = 0;
    if (_warnDue(lastCsrfWarnMs)) {
      Log::kvfw(TAG, "web.basic_refused reason=cross_site method=%s url=%s",
                req->methodToString(), req->url().c_str());
    }
  }

  // Skip the Basic check when Bearer already verified (or a veto decides).
  const bool basicOk = !passIsDefault && !bearerValid && basicAllowed &&
                       req->authenticate(webUser, webPass);
  mbedtls_platform_zeroize(passBuf, sizeof(passBuf));
  const bool allowed = webAuthAllowed(passIsDefault, bearerValid, basicOk);
  // A policy veto is not a wrong guess: the right credential gets it too.
  if (guessed) {
    *guessed = webAuthCountsAsGuess(allowed, basicAllowed && !passIsDefault,
                                    basicOffered || bearerOffered);
  }
  return allowed;
}

enum RouteAuth { ROUTE_AUTH_OK, ROUTE_AUTH_DENIED, ROUTE_AUTH_LOCKED };

// Every route handler authenticates here: lockout first, one count per offered wrong credential.
static RouteAuth _authCounted(AsyncWebServerRequest* req, bool hasSideEffect = false) {
  const String ip = req->client()->remoteIP().toString();
  if (!_rlAllow(ip)) return ROUTE_AUTH_LOCKED;
  bool guessed = false;
  if (!_checkAuth(req, hasSideEffect, &guessed)) {
    if (guessed) _rlFail(ip);
    return ROUTE_AUTH_DENIED;
  }
  _rlReset(ip);
  return ROUTE_AUTH_OK;
}

// True when authorized, otherwise replies 429 (locked out) or 401.
static bool _authOrReply(AsyncWebServerRequest* req, bool hasSideEffect = false) {
  const RouteAuth a = _authCounted(req, hasSideEffect);
  if (a == ROUTE_AUTH_OK) return true;
  if (a == ROUTE_AUTH_LOCKED) {
    req->send(429, "application/json", "{\"ok\":false,\"error\":\"Too many attempts - wait 30s\"}");
  } else {
    req->send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}");
  }
  return false;
}

// POST bodies live in req->_tempObject, one per request. Chunk 0 authenticates and counts
// once, so a client that drops mid-body still spends its guess. Refused bodies are never buffered.
// Bytes the live bodies reserved. async_tcp only: the body chunks and the
// disconnect that ends every request both run there.
static size_t _bodyHeld = 0;

static void _bodyChunk(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                       size_t index, size_t total, size_t maxBytes) {
  if (index == 0) {
    const RouteAuth a = _authCounted(req);
    if (a != ROUTE_AUTH_OK) {
      req->_tempObject = httpBodyDenied(a == ROUTE_AUTH_LOCKED);
    } else if (total > maxBytes || !httpBodyReserve(_bodyHeld, total, HTTP_BODY_BUDGET)) {
      if (total <= maxBytes) Log::kvfw(TAG, "web.body_refused reason=budget len=%u held=%u",
                                       (unsigned)total, (unsigned)_bodyHeld);
      req->_tempObject = nullptr;
    } else {
      req->_tempObject = httpBodyBegin(total, maxBytes);
      if (!req->_tempObject) httpBodyRelease(_bodyHeld, total);
      else req->onDisconnect([total]() { httpBodyRelease(_bodyHeld, total); });
    }
  }
  httpBodyAppend((HttpBody*)req->_tempObject, data, len, index);
}

// The chunk-0 verdict, without counting again: the guess that starts a
// lockout still answers 401, like on the other routes.
static void _replyDenied(AsyncWebServerRequest* req, bool locked) {
  if (locked) {
    req->send(429, "application/json", "{\"ok\":false,\"error\":\"Too many attempts - wait 30s\"}");
  } else {
    req->send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}");
  }
}

// The whole body ("" for Content-Length 0), or nullptr after replying 401,
// 413 past the cap, or 400 when it did not arrive whole.
static const char* _bodyOrReply(AsyncWebServerRequest* req, size_t maxBytes) {
  const HttpBody* b = (const HttpBody*)req->_tempObject;
  if (b && b->denied) { _replyDenied(req, b->locked); return nullptr; }
  const RouteAuth a = _authCounted(req);
  if (a == ROUTE_AUTH_LOCKED) {
    req->send(429, "application/json", "{\"ok\":false,\"error\":\"Too many attempts - wait 30s\"}");
    return nullptr;
  }
  switch (httpBodyVerdict(a == ROUTE_AUTH_OK, req->hasHeader("Transfer-Encoding"),
                          req->hasHeader("Content-Length"), req->contentLength(),
                          maxBytes, b)) {
    case HTTP_BODY_OK:
      return b->data;
    case HTTP_BODY_EMPTY:
      return "";
    case HTTP_BODY_UNAUTHORIZED:
      req->send(401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized\"}");
      return nullptr;
    case HTTP_BODY_LENGTH_REQUIRED:
      req->send(411, "application/json", "{\"ok\":false,\"error\":\"Content-Length required, chunked or unsized bodies are refused\"}");
      return nullptr;
    case HTTP_BODY_TOO_LARGE:
      req->send(413, "application/json", "{\"ok\":false,\"error\":\"body too large\"}");
      return nullptr;
    case HTTP_BODY_INCOMPLETE:
      if (!b) {
        req->send(503, "application/json", "{\"ok\":false,\"error\":\"busy, retry\"}");
        return nullptr;
      }
      break;
  }
  req->send(400, "application/json", "{\"ok\":false,\"error\":\"body not received\"}");
  return nullptr;
}

// Write len bytes to path through ".upload.tmp" in the same directory, so a
// failed write leaves the old file. in: fs, path, body. out: bytes on disk.
static bool _fileWriteAtomic(bool lfs, const char* path, const char* data, size_t len,
                             size_t* onDisk) {
  char tmp[192];
  char bak[192];
  if (!uploadTmpPath(tmp, sizeof(tmp), path) ||
      !uploadStagePath(bak, sizeof(bak), path, UPLOAD_BAK_NAME)) return false;
  File f = lfs ? LittleFS.open(tmp, FILE_WRITE) : SD_MMC.open(tmp, FILE_WRITE);
  if (!f) return false;
  size_t written = len ? f.write((const uint8_t*)data, len) : 0;
  f.close();
  // fclose does the real write and drops its result. Read the size back.
  size_t got = 0;
  File chk = lfs ? LittleFS.open(tmp, FILE_READ) : SD_MMC.open(tmp, FILE_READ);
  if (chk) { got = chk.size(); chk.close(); }
  if (!fileWriteLanded(written, got, len)) {
    if (lfs) LittleFS.remove(tmp); else SD_MMC.remove(tmp);
    return false;
  }
  if (lfs) {
    // lfs rename replaces the target atomically.
    if (!LittleFS.rename(tmp, path)) { LittleFS.remove(tmp); return false; }
    *onDisk = got;
    return true;
  }
  // FAT cannot rename onto an existing name, so the old file is parked as
  // .upload.bak first (fatCommit). Some copy survives every failing step.
  struct SdOps {
    bool exists(const char* p) { return SD_MMC.exists(p); }
    bool remove(const char* p) { return SD_MMC.remove(p); }
    bool rename(const char* a, const char* b) { return SD_MMC.rename(a, b); }
  } sd;
  switch (fatCommit(sd, tmp, bak, path)) {
    case FAT_COMMIT_OK:
      break;
    case FAT_COMMIT_OK_BAK_LEFT:
      Log::kvfw(TAG, "web.file_bak_left path=%s", bak);
      break;
    case FAT_COMMIT_STRANDED:
      Log::kvfe(TAG, "web.file_restore_failed path=%s old=%s new=%s", path, bak, tmp);
      return false;
    case FAT_COMMIT_KEPT_OLD:
      return false;
  }
  *onDisk = got;
  return true;
}

// Parse-check a config body, then write the file only (tmp + rename) on the
// main loop task. Replies on failure. out: bytes on disk, 0 if nothing landed.
static size_t _configWriteOrReply(AsyncWebServerRequest* req, const char* body, bool restart) {
  // One config cap for every route, /api/file buffers up to the file cap.
  if (req->contentLength() > HTTP_BODY_CFG_MAX) {
    req->send(413, "application/json", "{\"ok\":false,\"error\":\"config larger than 16 KB\"}");
    return 0;
  }
  auto doc = std::make_shared<JsonDocument>();
  if (deserializeJson(*doc, body) || !doc->is<JsonObject>()) {
    req->send(400, "application/json", "{\"ok\":false,\"error\":\"config must be a JSON object\"}");
    return 0;
  }
  auto done  = std::make_shared<std::atomic<bool>>(false);
  auto bytes = std::make_shared<size_t>(0);
  bool queued = Shell::enqueueDeferred([doc, done, bytes, restart]() {
    bool ok = Config::writeFile(doc->as<JsonVariantConst>(), true, bytes.get());
    if (!ok) *bytes = 0;
    // Set here, not after the wait, so a 504 still restarts into what landed.
    if (ok && restart) _restartPending = true;
    *done = true;
  });
  if (!queued) {
    req->send(503, "application/json", "{\"ok\":false,\"error\":\"shell busy\"}");
    return 0;
  }
  // Same 5 s spin-yield as /api/cmd while the main loop drains the ring.
  for (uint32_t i = 0; i < 1000 && !*done; i++) delay(5);
  if (!*done) {
    req->send(504, "application/json", "{\"ok\":false,\"error\":\"config write timeout, result unknown\"}");
    return 0;
  }
  if (*bytes == 0) {
    req->send(500, "application/json", "{\"ok\":false,\"error\":\"config write failed\"}");
    return 0;
  }
  return *bytes;
}

// Single-use WS grants (ws_grant_policy.h): /api/ws/token mints a random token
// bound to the caller's IP, the /ws/serial handshake spends it.
static WsGrant      _wsGrants[WS_GRANT_SLOTS] = {};
static portMUX_TYPE _wsGrantMux = portMUX_INITIALIZER_UNLOCKED;

// in: issuing IP, out buffer of WS_TOKEN_HEX + 1. out: false when the table is full.
static bool _issueWsGrant(const String& ip, char* token) {
  uint8_t raw[WS_TOKEN_HEX / 2];
  esp_fill_random(raw, sizeof(raw));
  wsTokenHex(raw, sizeof(raw), token);
  mbedtls_platform_zeroize(raw, sizeof(raw));
  portENTER_CRITICAL(&_wsGrantMux);
  const int slot = wsGrantIssue(_wsGrants, _uptimeMs(), token, ip.c_str());
  portEXIT_CRITICAL(&_wsGrantMux);
  return slot >= 0;
}

static bool _consumeWsGrant(const char* token, const String& ip) {
  portENTER_CRITICAL(&_wsGrantMux);
  const bool ok = wsGrantConsume(_wsGrants, _uptimeMs(), token, ip.c_str());
  portEXIT_CRITICAL(&_wsGrantMux);
  return ok;
}

// ---------------------------------------------------------------------------
// Embedded dashboard HTML (served from PROGMEM - see dashboard.html.h).
// ---------------------------------------------------------------------------
#include "dashboard.html.h"

// ---------------------------------------------------------------------------

// Start the HTTP server, register routes, and hook up log broadcasting
void HttpServer::begin() {
  if (!LittleFS.begin()) {
    Log::warn(TAG, "web.littlefs_unmounted config_editor=read_only");
  }

  // Relay log lines to WebSocket terminal clients.
  Log::setRemoteHandler(&HttpServer::broadcastLog);

  _webSnapRefresh();
  // pumpConsole can run config.set deep inside OTA or modem waits, far from loop().
  if (!Config::onChange(_webSnapRefresh)) Log::error(TAG, "web.snapshot_hook_failed");
  subscribeToEvents();
  setupRoutes();

  // Routes are registered either way; only the bind waits. The fallback AP
  // brings a netif up later, and loop() picks it up then.
  if (!netifUp()) {
    Log::warn(TAG, "web.server_deferred reason=no_netif");
    return;
  }
  server.begin();
  _serverStarted = true;
  Log::info(TAG, "web.server_started port=80");
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
  if (!_webSnapSet || Config::revision() != _webSnapRev) _webSnapRefresh();
  SetupPortal::loop();
  if (!_serverStarted) {
    if (!netifUp()) return;
    server.begin();
    _serverStarted = true;
    Log::info(TAG, "web.server_started port=80 deferred=true");
  }
  _ws.cleanupClients();  // required by ESPAsyncWebServer 3.x to drain the WS send queue
  if (_restartPending) {
    Log::info(TAG, "web.restart reason=api_request");
    delay(500);
    OTAUpdate::plannedRestart();
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
  const size_t need = measureJson(_stateDoc);
  if (need >= STATE_JSON_MAX) {
    static uint32_t lastWarnMs = 0;
    if (_warnDue(lastWarnMs)) Log::kvfw(TAG, "web.state_too_big bytes=%u cap=%u", (unsigned)need, (unsigned)STATE_JSON_MAX);
    return;
  }
  char* fresh = (char*)malloc(need + 1);
  if (!fresh) return;
  const size_t n = serializeJson(_stateDoc, fresh, need + 1);
  taskENTER_CRITICAL(&_stateMux);
  char* old = _stateJson;
  _stateJson = fresh;
  _stateJsonLen = n;
  taskEXIT_CRITICAL(&_stateMux);
  free(old);
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

static const char kSetupFields[] =
  "<h1>thesada setup</h1><form method=\"post\" action=\"/setup\">"
  "<p>WiFi name<br><input name=\"ssid\" maxlength=\"32\" required></p>"
  "<p>Passphrase<br><input name=\"pass\" type=\"password\" minlength=\"8\" maxlength=\"63\" required></p>"
  "<p>App URL<br><input name=\"url\" value=\"https://thesada.app\" maxlength=\"127\" required></p>"
  "<p><button type=\"submit\">Save</button></p></form>";

static const char kSetupWait[] =
  "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
  "<meta http-equiv=\"refresh\" content=\"1;url=/setup\"><title>thesada setup</title></head>"
  "<body><p>Saving. This page reloads in a moment.</p></body></html>";

// One setup document. extra is already escaped, or null for the bare form.
// in: optional paragraph. out: full HTML page.
static String setupDocument(const char* extra) {
  String page = "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                "<title>thesada setup</title></head><body>";
  if (extra && extra[0]) {
    page += "<p>";
    page += extra;
    page += "</p>";
  }
  page += kSetupFields;
  page += "</body></html>";
  return page;
}

// Escape text for an HTML body. Drops controls and non-ASCII.
// in: raw text. out: escaped text, possibly empty.
static String htmlText(const char* in) {
  String out;
  if (!in) return out;
  for (const unsigned char* p = (const unsigned char*)in; *p; p++) {
    if (*p == '&') out += "&amp;";
    else if (*p == '<') out += "&lt;";
    else if (*p == '>') out += "&gt;";
    else if (*p == '"') out += "&quot;";
    else if (*p >= 0x20 && *p < 0x7f) out += (char)*p;
  }
  return out;
}

// Copy one POST field. Rejects a value that does not fit, so a truncated
// passphrase is never stored. in: request, field, dest, cap. out: copied.
static bool copyPostField(AsyncWebServerRequest* req, const char* name,
                          char* dst, size_t cap) {
  dst[0] = '\0';
  if (!req->hasParam(name, true) || cap == 0) return false;
  const String& v = req->getParam(name, true)->value();
  if ((size_t)v.length() >= cap) return false;
  memcpy(dst, v.c_str(), v.length() + 1);
  return true;
}

// Serve the setup form, the wait page, or the claim link. in: request. out: none.
static void sendSetupPage(AsyncWebServerRequest* req) {
  if (!SetupPortal::open()) {
    req->send(404, "text/plain", "Not found");
    return;
  }
  if (SetupPortal::busy()) {
    req->send(200, "text/html", kSetupWait);
    return;
  }
  char link[SetupPortal::LINK_CAP];
  const char* err = "";
  SetupPortal::result(link, sizeof(link), &err);
  if (SetupPortal::hasResult() && link[0]) {
    String page = "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                  "<title>thesada setup</title></head><body>"
                  "<p>Claim this device:</p><pre>";
    page += htmlText(link);
    page += "</pre></body></html>";
    req->send(200, "text/html", page);
    return;
  }
  if (err && err[0]) {
    String safe = htmlText(err);
    req->send(200, "text/html", setupDocument(safe.c_str()));
    return;
  }
  req->send(200, "text/html", setupDocument(nullptr));
}

// First in the handler list, so it judges every request once its headers are
// in. It claims a refused one and, being trivial, makes the library skip its
// body bytes instead of parsing them (ESPAsyncWebServer 3.12.1 _onData).
class ParsedBodyGuard : public AsyncWebHandler {
 public:
  bool canHandle(AsyncWebServerRequest* req) const override {
    return verdict(req) != HTTP_PARSED_PASS;
  }
  bool isRequestHandlerTrivial() const override { return true; }
  void handleRequest(AsyncWebServerRequest* req) override {
    const HttpParsedBodyVerdict v = verdict(req);
    Log::kvfw(TAG, "web.body_refused reason=%s len=%u",
              v == HTTP_PARSED_TOO_LARGE ? "form_too_large" : "multipart",
              (unsigned)req->contentLength());
    if (v == HTTP_PARSED_TOO_LARGE) {
      req->send(413, "application/json", "{\"ok\":false,\"error\":\"form body too large\"}");
    } else {
      req->send(415, "application/json", "{\"ok\":false,\"error\":\"multipart only on /ota\"}");
    }
  }

 private:
  static HttpParsedBodyVerdict verdict(AsyncWebServerRequest* req) {
    const bool upload = req->method() == HTTP_POST && req->url() == "/ota";
    return httpParsedBodyVerdict(req->contentType().c_str(), req->contentLength(),
                                 req->multipart(), upload);
  }
};

// Register all HTTP routes, WebSocket handler, OTA endpoint, and captive portal
void HttpServer::setupRoutes() {
  server.addHandler(new ParsedBodyGuard());

  // Credentials are resolved per request in _checkAuth; check once here only
  // for the boot-time default-password warning.
  const WebCredState bootCred = _webCredLock();
  if (bootCred != WEB_CRED_OK) {
    Log::kvfw(TAG, "web.admin_locked reason=%s",
              bootCred == WEB_CRED_TOO_LONG ? "credential_too_long" : "default_password");
  }

  // ── Dashboard HTML - public (sensor data is read-only) ────────────────────
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (SetupPortal::open()) {
      req->redirect("/setup");
      return;
    }
    AsyncWebServerResponse* resp = req->beginResponse(200, "text/html", (const uint8_t*)DASHBOARD_HTML, strlen_P(DASHBOARD_HTML));
    req->send(resp);
  });

  server.on("/setup", HTTP_GET, [](AsyncWebServerRequest* req) {
    sendSetupPage(req);
  });

  server.on("/setup", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!SetupPortal::open()) {
      req->send(404, "text/plain", "Not found");
      return;
    }
    char ssid[33] = {};
    char pass[64] = {};
    char url[ENROLL_URL_CAP] = {};
    bool queued = copyPostField(req, "ssid", ssid, sizeof(ssid)) &&
                  copyPostField(req, "pass", pass, sizeof(pass)) &&
                  copyPostField(req, "url", url, sizeof(url)) &&
                  SetupPortal::submit(ssid, pass, url);
    mbedtls_platform_zeroize(pass, sizeof(pass));
    if (!queued) {
      req->send(400, "text/plain", "rejected");
      return;
    }
    req->send(200, "text/html", kSetupWait);
  });

  // ── GET /api/auth/check - credential verification (plain 200 or 401) ─────
  // Must NOT call requestAuthentication() here - that would send WWW-Authenticate
  // which causes the browser to silently retry with cached credentials, bypassing
  // the JS modal check entirely.
  server.on("/api/auth/check", HTTP_GET, [](AsyncWebServerRequest* req) {
    String ip = req->client()->remoteIP().toString();
    if (!_rlAllow(ip)) {
      req->send(429, "application/json", "{\"ok\":false,\"error\":\"Too many attempts - wait 30s\"}");
      return;
    }
    // Say WHY when the admin surface is locked by the default password -
    // otherwise the login modal just loops with no way to tell. 403 (not
    // 401): no credentials can succeed until web.password is set.
    if (_sendCredLocked(req, _webCredLock())) return;
    // Side-effect GET: answers "are these cached credentials valid here" and
    // moves the rate-limit counter, so a cross-site caller gets no oracle.
    if (!_authOrReply(req, /*hasSideEffect=*/true)) return;
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // ── POST /api/login - exchange Basic Auth for a Bearer token ────────────────
  server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest* req) {
    String ip = req->client()->remoteIP().toString();
    if (!_rlAllow(ip)) {
      req->send(429, "application/json", "{\"ok\":false,\"error\":\"Too many attempts - wait 30s\"}");
      return;
    }
    if (_sendCredLocked(req, _webCredLock())) return;
    if (!_authOrReply(req, /*hasSideEffect=*/false)) return;
    const char* token = _createToken();
    char body[128];
    snprintf(body, sizeof(body),
      "{\"ok\":true,\"token\":\"%s\",\"expires_in\":%lu}",
      token, (unsigned long)(TOKEN_TTL_MS / 1000));
    req->send(200, "application/json", body);
    Log::info(TAG, "web.token_issued");
  });

  // ── GET /api/ws/token - issue a single-use WS token (auth required) ───────
  // Clients pass it as /ws/serial?token=<hex>. Side-effect GET: it mints a
  // grant and /ws/serial reaches the shell, so no cached cross-site Basic.
  server.on("/api/ws/token", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!_authOrReply(req, /*hasSideEffect=*/true)) return;
    char token[WS_TOKEN_HEX + 1];
    if (!_issueWsGrant(req->client()->remoteIP().toString(), token)) {
      Log::warn(TAG, "web.ws_token_refused reason=table_full");
      req->send(503, "application/json", "{\"ok\":false,\"error\":\"too many pending terminal logins, retry in 30 s\"}");
      return;
    }
    char body[64];
    snprintf(body, sizeof(body), "{\"ok\":true,\"token\":\"%s\"}", token);
    mbedtls_platform_zeroize(token, sizeof(token));
    req->send(200, "application/json", body);
    mbedtls_platform_zeroize(body, sizeof(body));
  });

  // ── GET /api/info - firmware version + build info (no auth) ──────────────
  server.on("/api/info", HTTP_GET, [](AsyncWebServerRequest* req) {
    WebSnap snap;
    _webSnapRead(snap);
    mbedtls_platform_zeroize(snap.passCfg, sizeof(snap.passCfg));
    const char* devName = snap.devName;
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
    // Size first, then copy; a snapshot that grew in between gets one more try.
    char* snap = nullptr;
    for (int attempt = 0; attempt < 3 && !snap; attempt++) {
      taskENTER_CRITICAL(&_stateMux);
      const size_t len = _stateJsonLen;
      taskEXIT_CRITICAL(&_stateMux);
      char* buf = (char*)malloc(len + 1);
      if (!buf) { req->send(503, "application/json", "{\"error\":\"out of memory\"}"); return; }
      bool copied = false;
      taskENTER_CRITICAL(&_stateMux);
      if (!_stateJson) {
        buf[0] = '\0';
        copied = true;
      } else if (_stateJsonLen <= len) {
        memcpy(buf, _stateJson, _stateJsonLen + 1);
        copied = true;
      }
      taskEXIT_CRITICAL(&_stateMux);
      if (copied) snap = buf; else free(buf);
    }
    if (!snap) { req->send(503, "application/json", "{\"error\":\"state busy\"}"); return; }
    JsonDocument resp;
    const DeserializationError err = deserializeJson(resp, snap);
    free(snap);
    if (err || !resp.is<JsonObject>()) resp.to<JsonObject>();
    JsonObject mqttMeta = resp["_mqtt"].to<JsonObject>();
    mqttMeta["connected"] = MQTTClient::connectedCached();
    time_t lp = MQTTClient::lastPublishTime();
    if (lp > 1700000000UL) {
      char ts[22];
      struct tm tmv;
      gmtime_r(&lp, &tmv);
      strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);
      mqttMeta["last_publish"] = ts;
    } else {
      mqttMeta["last_publish"] = nullptr;
    }
    String out;
    serializeJson(resp, out);
    req->send(200, "application/json", out);
  });

  // ── GET /api/config - read config.json ────────────────────────────────────
  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!_authOrReply(req)) return;
    if (!LittleFS.exists("/config.json")) {
      req->send(404, "application/json", "{\"error\":\"config.json not found\"}");
      return;
    }
    req->send(LittleFS, "/config.json", "application/json");
  });

  // ── POST /api/config - write config.json (auth required) ──────────────────
  // Body is buffered per request after auth on chunk 0. Only a whole body
  // that parses as an object is written, and only a written one restarts.
  AsyncCallbackWebHandler* cfgHandler = new AsyncCallbackWebHandler();
  cfgHandler->setUri("/api/config");
  cfgHandler->setMethod(HTTP_POST);
  cfgHandler->onBody([](AsyncWebServerRequest* req, uint8_t* data, size_t len,
                        size_t index, size_t total) {
    _bodyChunk(req, data, len, index, total, HTTP_BODY_CFG_MAX);
  });
  cfgHandler->onRequest([](AsyncWebServerRequest* req) {
    const char* body = _bodyOrReply(req, HTTP_BODY_CFG_MAX);
    if (!body) return;
    if (_configWriteOrReply(req, body, /*restart=*/true) == 0) return;
    req->send(200, "application/json", "{\"ok\":true}");
  });
  server.addHandler(cfgHandler);

  // ── POST /api/backup - copy config.json to SD (auth required) ────────────
  server.on("/api/backup", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!_authOrReply(req)) return;
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
    uint8_t buf[512];
    bool    copied = true;
    for (int n; copied && (n = src.read(buf, sizeof(buf))) > 0;) {
      copied = dst.write(buf, (size_t)n) == (size_t)n;
    }
    src.close(); dst.close();
    if (!copied) {
      Log::kvfe(TAG, "web.config_backup_failed reason=short_write");
      req->send(200, "application/json", "{\"ok\":false,\"error\":\"SD write failed\"}");
      return;
    }
    Log::info(TAG, "web.config_backup dest=sd:/config_backup.json");
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // ── POST /api/restart (auth required) ─────────────────────────────────────
  server.on("/api/restart", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!_authOrReply(req)) return;
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
    _bodyChunk(req, data, len, index, total, HTTP_BODY_CMD_MAX);
  });
  cmdHandler->onRequest([](AsyncWebServerRequest* req) {
    const char* body = _bodyOrReply(req, HTTP_BODY_CMD_MAX);
    if (!body) return;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err || !doc["cmd"].is<const char*>()) {
      req->send(400, "application/json", "{\"ok\":false,\"error\":\"expected {\\\"cmd\\\":\\\"...\\\"}\"}");
      return;
    }
    String cmdStr = doc["cmd"].as<String>();
    if (!shellModeHttpSurfaceAllowed(Shell::mode(), HTTP_SURFACE_CMD)) {
      req->send(403, "application/json",
                "{\"ok\":false,\"error\":\"shell.mode closes the HTTP command surface\"}");
      return;
    }

    // Dispatch through the Shell deferred ring so the actual Shell::execute
    // runs on the main-loop task instead of inside the AsyncTCP onRequest
    // callback. The async task stack is sized for WS-frame dispatch only -
    // any shell command that reaches LittleFS / MQTT / TLS / Lua exec can
    // overflow it from here (same risk that motivated the earlier WS
    // dispatch refactor).
    //
    // Shared state: output buffer + done flag carried by shared_ptr so the
    // capture survives this callback's return. Done flag flips after the
    // ring drains the lambda - on the AsyncTCP task we spin-poll with
    // delay() yielding to other tasks (including main, which is the one
    // doing the drain) until either done flips or the timeout expires.
    auto output = std::make_shared<String>("[");
    auto first  = std::make_shared<bool>(true);
    auto done   = std::make_shared<std::atomic<bool>>(false);
    auto cmdOk  = std::make_shared<std::atomic<bool>>(false);

    bool ok = Shell::enqueueDeferred([cmdStr, output, first, done, cmdOk]() {
      ShellResult r = Shell::execute(cmdStr.c_str(), [output, first](const char* line) {
        if (!*first) *output += ",";
        *output += "\"";
        for (const char* p = line; *p; ++p) {
          if      (*p == '"')  *output += "\\\"";
          else if (*p == '\\') *output += "\\\\";
          else if (*p == '\n') *output += "\\n";
          else if (*p == '\r') *output += "\\r";
          else                 *output += *p;
        }
        *output += "\"";
        *first = false;
      });
      *output += "]";
      *cmdOk = (r == ShellResult::Ok);
      *done = true;
    });
    if (!ok) {
      req->send(503, "application/json", "{\"ok\":false,\"error\":\"shell busy\"}");
      return;
    }

    // Spin-yield up to 5 s for the ring to drain. AsyncTCP task yielding
    // here lets the main loop's Shell::loop() pick up our slot. delay(5)
    // ~= 5 ms FreeRTOS tick yield. 1000 iters = 5 s ceiling - matches the
    // WiFi/MQTT timeouts a single CLI command is allowed to consume.
    const uint32_t pollMs   = 5;
    const uint32_t maxPolls = 1000;
    for (uint32_t i = 0; i < maxPolls && !*done; i++) delay(pollMs);

    if (!*done) {
      req->send(504, "application/json",
                "{\"ok\":false,\"error\":\"shell timeout (5s)\"}");
      return;
    }

    String resp = *cmdOk ? "{\"ok\":true,\"output\":" : "{\"ok\":false,\"output\":";
    resp += *output;
    resp += "}";
    req->send(200, "application/json", resp);
  });
  server.addHandler(cmdHandler);

  // ── POST /ota - firmware binary upload (auth required) ────────────────────
  // Accepts multipart/form-data with field "firmware". Uses ESP-IDF Update.h.
  server.on("/ota", HTTP_POST,
    [](AsyncWebServerRequest* req) {
      const uint8_t* mark = (const uint8_t*)req->_tempObject;
      if (mark && *mark != ROUTE_AUTH_OK) { _replyDenied(req, *mark == ROUTE_AUTH_LOCKED); return; }
      // An accepted chunk 0 settles auth: a lockout started meanwhile by other
      // requests must not skip the restart into the image just flashed.
      if (!mark && !_authOrReply(req)) return;
      // No authorized-upload marker = no chunk was ever accepted (empty body
      // or marker alloc failure) - nothing was flashed, don't restart.
      if (req->_tempObject == nullptr) { req->send(400, "application/json", "{\"ok\":false,\"error\":\"no firmware received\"}"); return; }
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
      // Upload chunks arrive BEFORE the onRequest auth check fires, so auth
      // must be enforced here. _tempObject doubles as the per-request
      // marker: the chunk-0 RouteAuth on the heap, freed by the request destructor.
      // Only ROUTE_AUTH_OK lets a chunk through, NULL drops every chunk.
      if (index == 0) {
        uint8_t* mark = (uint8_t*)malloc(1);
        if (mark) *mark = (uint8_t)_authCounted(req);
        req->_tempObject = mark;
      }
      if (req->_tempObject == nullptr || *(const uint8_t*)req->_tempObject != ROUTE_AUTH_OK) {
        if (index == 0) Log::warn(TAG, "web.ota_upload_refused reason=unauthorized");
        return;
      }
      if (index == 0) {
        // Use content length from request if available, otherwise unknown
        size_t totalSize = req->contentLength();
        Log::kvf(TAG, "web.ota_upload_start file=%s bytes=%u",
                 filename.c_str(), (unsigned)totalSize);
        if (!Update.begin(totalSize > 0 ? totalSize : UPDATE_SIZE_UNKNOWN)) {
          Log::kvfe(TAG, "web.ota_begin_failed err=\"%s\"", Update.errorString());
        }
      }
      if (Update.isRunning()) {
        if (Update.write(data, len) != len) {
          Log::kvfe(TAG, "web.ota_write_failed err=\"%s\"", Update.errorString());
        }
      }
      if (final) {
        if (Update.end(true)) {
          Log::kvf(TAG, "web.ota_upload_done bytes=%u", (unsigned)(index + len));
        } else {
          Log::kvfe(TAG, "web.ota_upload_failed err=\"%s\"", Update.errorString());
        }
      }
    }
  );

  // ── GET /api/files - list files (auth required) ──────────────────────────
  // Query param: source=sd (default) or source=littlefs or source=scripts
  server.on("/api/files", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!_authOrReply(req)) return;
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

  // ── GET /api/file?path=...&source=sd|littlefs|scripts - stream a file, any size (auth)
  server.on("/api/file", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!_authOrReply(req)) return;
    if (!req->hasParam("path")) { req->send(400, "text/plain", "missing path"); return; }
    String path = req->getParam("path")->value();
    if (!_pathSafe(path)) { req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid path\"}"); return; }
    String src = req->hasParam("source") ? req->getParam("source")->value() : "sd";
    bool useLFS     = (src == "littlefs");
    bool useScripts = (src == "scripts");
    if (useScripts) path = "/scripts" + path;
    File f = (useLFS || useScripts) ? LittleFS.open(path.c_str(), FILE_READ) : SD_MMC.open(path.c_str(), FILE_READ);
    if (!f || f.isDirectory()) { req->send(404, "text/plain", "not found"); return; }
    // Streamed whole, never clipped. The response owns f and closes it.
    req->send(f, path, "text/plain");
  });

  // ── DELETE /api/file?path=...&source=sd|littlefs|scripts - delete file (auth)
  server.on("/api/file", HTTP_DELETE, [](AsyncWebServerRequest* req) {
    if (!_authOrReply(req)) return;
    if (!shellModeHttpSurfaceAllowed(Shell::mode(), HTTP_SURFACE_FILE_WRITE)) {
      req->send(403, "application/json", "{\"ok\":false,\"error\":\"shell.mode closes file writes over HTTP\"}");
      return;
    }
    if (!req->hasParam("path")) { req->send(400, "application/json", "{\"error\":\"missing path\"}"); return; }
    String path = req->getParam("path")->value();
    if (!_pathSafe(path)) { req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid path\"}"); return; }
    String src = req->hasParam("source") ? req->getParam("source")->value() : "sd";
    bool useLFS     = (src == "littlefs");
    bool useScripts = (src == "scripts");
    if (useScripts) path = "/scripts" + path;
    bool ok = (useLFS || useScripts) ? LittleFS.remove(path.c_str()) : SD_MMC.remove(path.c_str());
    req->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"delete failed\"}");
  });

  // ── POST /api/file?path=... - write a file, tmp + rename (auth required) ─
  AsyncCallbackWebHandler* fileHandler = new AsyncCallbackWebHandler();
  fileHandler->setUri("/api/file");
  fileHandler->setMethod(HTTP_POST);
  fileHandler->onBody([](AsyncWebServerRequest* req, uint8_t* data, size_t len,
                         size_t index, size_t total) {
    _bodyChunk(req, data, len, index, total, HTTP_BODY_FILE_MAX);
  });
  fileHandler->onRequest([](AsyncWebServerRequest* req) {
    // Body before the open: a body that did not arrive must not truncate the file.
    const char* body = _bodyOrReply(req, HTTP_BODY_FILE_MAX);
    if (!body) return;
    if (!shellModeHttpSurfaceAllowed(Shell::mode(), HTTP_SURFACE_FILE_WRITE)) {
      req->send(403, "application/json", "{\"ok\":false,\"error\":\"shell.mode closes file writes over HTTP\"}");
      return;
    }
    if (!req->hasParam("path")) {
      req->send(400, "application/json", "{\"error\":\"missing path\"}");
      return;
    }
    String path    = req->getParam("path")->value();
    if (!_pathSafe(path)) {
      req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid path\"}");
      return;
    }
    String src     = req->hasParam("source") ? req->getParam("source")->value() : "sd";
    bool   useLFS     = (src == "littlefs");
    bool   useScripts = (src == "scripts");
    if (useScripts) path = "/scripts" + path;
    // The config file never takes an in-place write. Live config is untouched.
    if (useLFS && configPathIsLive(path.c_str())) {
      size_t n = _configWriteOrReply(req, body, /*restart=*/false);
      if (n == 0) return;
      char cmsg[64];
      snprintf(cmsg, sizeof(cmsg), "{\"ok\":true,\"bytes\":%u}", (unsigned)n);
      req->send(200, "application/json", cmsg);
      return;
    }
    // Content-Length 0 is a legitimate empty file (editor cleared and saved).
    // The declared length, not strlen, so a NUL in the body is written too.
    size_t written = 0;
    if (!_fileWriteAtomic(useLFS || useScripts, path.c_str(), body, req->contentLength(), &written)) {
      req->send(200, "application/json", (useLFS || useScripts) ? "{\"ok\":false,\"error\":\"LittleFS write failed\"}"
                                                                 : "{\"ok\":false,\"error\":\"SD write failed\"}");
      return;
    }
    char rmsg[64];
    snprintf(rmsg, sizeof(rmsg), "{\"ok\":true,\"bytes\":%d}", (int)written);
    req->send(200, "application/json", rmsg);
  });
  server.addHandler(fileHandler);

  // ── WebSocket /ws/serial - bidirectional serial terminal ──────────────────
  // All commands are handled by the Shell core class.
  // The upgrade is refused before a client exists, so a refused socket never
  // joins textAll and gets no log line. 401 on refusal (library default).
  _ws.handleHandshake([](AsyncWebServerRequest* req) -> bool {
    // Before the grant is spent: a narrowed shell.mode allows no session.
    if (!shellModeHttpSurfaceAllowed(Shell::mode(), HTTP_SURFACE_TERMINAL)) {
      Log::kvfw(TAG, "web.ws_rejected reason=shell_mode");
      return false;
    }
    const AsyncWebParameter* p = req->getParam("token");
    if (!p) {
      Log::warn(TAG, "web.ws_rejected reason=no_token");
      return false;
    }
    if (!_consumeWsGrant(p->value().c_str(), req->client()->remoteIP().toString())) {
      Log::warn(TAG, "web.ws_rejected reason=bad_token");
      return false;
    }
    return true;
  });

  _ws.onEvent([](AsyncWebSocket* srv, AsyncWebSocketClient* client,
                 AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
      // The handshake already spent the grant. Replay the log ring as one
      // WS message, copied under the ring lock.
      char* replay = (char*)malloc(Log::RING_SNAPSHOT_CAP);
      if (replay) {
        const size_t n = Log::ringSnapshot(replay, Log::RING_SNAPSHOT_CAP);
        if (n > 0) client->text(replay, n);
        free(replay);
      } else {
        // A shorter copy would keep the oldest lines, so say why there is no history.
        Log::kvfw(TAG, "web.ws_replay_skipped reason=no_memory bytes=%u", (unsigned)Log::RING_SNAPSHOT_CAP);
        client->text("[log history skipped: low memory, new lines still stream]");
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
        // Stage on the Shell deferred ring instead of running on
        // the AsyncTCP task. The async task stack is sized for WS frame
        // dispatch only - any command that reaches LittleFS / MQTT / TLS
        // can blow it. Look up the client by id at drain time so a
        // disconnect mid-execute degrades to a no-op (the AsyncWebSocket
        // returns nullptr for the closed id) instead of UAF on a stale
        // AsyncWebSocketClient* pointer captured here.
        if (!shellModeHttpSurfaceAllowed(Shell::mode(), HTTP_SURFACE_TERMINAL)) {
          client->text("[shell.mode closes the HTTP command surface]");
          return;
        }
        if (cmd.length() >= Shell::DEFERRED_LINE_LEN) {
          client->text("[line too long - not run]");
          return;
        }
        uint32_t cid = client->id();
        if (!Shell::enqueue(cmd.c_str(), [cid](const char* line) {
              AsyncWebSocketClient* c = _ws.client(cid);
              if (c) c->text(line);
            })) {
          client->text("[shell busy - command dropped]");
        }
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

  Log::info(TAG, "web.routes_registered");
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
  const char* configKey() override { return "web"; }
};

MODULE_REGISTER(HttpServerModule, PRIORITY_SERVICE)

#endif // ENABLE_WEBSERVER
