// thesada-fw - LiteServer.cpp
// Lightweight HTTP server using Arduino built-in WebServer (blocking, single-client).
// Provides push OTA, config editor, and WiFi AP provisioning for heap-constrained boards.
// SPDX-License-Identifier: GPL-3.0-only

#include <thesada_config.h>
#ifdef ENABLE_LITESERVER

#include "LiteServer.h"
#include <Config.h>
#include <Log.h>
#include <WiFiManager.h>
#include <ModuleRegistry.h>
#include <Module.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <ArduinoJson.h>
#include "liteserver.html.h"

static const char* TAG = "LiteServer";
static WebServer _server(80);

// Check Basic Auth - skip in AP mode (no credentials configured yet)
static bool checkAuth() {
  if (WiFiManager::isAPActive()) return true;
  JsonObject cfg = Config::get();
  const char* user = cfg["web"]["user"]     | "";
  const char* pass = cfg["web"]["password"] | "";
  if (strlen(user) == 0 && strlen(pass) == 0) return true;
  if (!_server.authenticate(user, pass)) {
    _server.requestAuthentication();
    return false;
  }
  return true;
}

// Serve the setup page from PROGMEM
static void handleRoot() {
  if (!checkAuth()) return;
  _server.send_P(200, "text/html", LITE_HTML);
}

// Scan visible WiFi networks and return as JSON array
static void handleScan() {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"";
    json += WiFi.SSID(i);
    json += "\",\"rssi\":";
    json += String(WiFi.RSSI(i));
    json += "}";
  }
  json += "]";
  WiFi.scanDelete();
  _server.send(200, "application/json", json);
}

// Return firmware version, device name, and heap info
static void handleInfo() {
  JsonDocument doc;
  doc["version"] = FIRMWARE_VERSION;
  JsonObject cfg = Config::get();
  doc["device"] = cfg["device"]["friendly_name"] | cfg["device"]["name"] | "thesada";
  doc["heap"] = ESP.getFreeHeap();
  doc["ip"] = WiFi.localIP().toString();
  char buf[192];
  serializeJson(doc, buf, sizeof(buf));
  _server.send(200, "application/json", buf);
}

// Return current config.json contents
static void handleConfigGet() {
  if (!checkAuth()) return;
  File f = LittleFS.open("/config.json", "r");
  if (!f) { _server.send(500, "text/plain", "config.json not found"); return; }
  _server.streamFile(f, "application/json");
  f.close();
}

// Save posted JSON body to config.json
static void handleConfigPost() {
  if (!checkAuth()) return;
  if (!_server.hasArg("plain")) { _server.send(400, "text/plain", "empty body"); return; }
  String body = _server.arg("plain");
  // Validate JSON before saving
  JsonDocument doc;
  if (deserializeJson(doc, body)) { _server.send(400, "text/plain", "invalid JSON"); return; }
  File f = LittleFS.open("/config.json", "w");
  if (!f) { _server.send(500, "text/plain", "write failed"); return; }
  f.print(body);
  f.close();
  Config::load();
  _server.send(200, "text/plain", "saved");
  Log::info(TAG, "Config saved via LiteServer");
}

// Save WiFi credentials and reboot
static void handleWifi() {
  if (!checkAuth()) return;
  String ssid = _server.arg("ssid");
  String pass = _server.arg("password");
  if (ssid.isEmpty()) { _server.send(400, "text/plain", "SSID required"); return; }

  // Read existing config, update wifi.networks[0], save
  File f = LittleFS.open("/config.json", "r");
  if (!f) { _server.send(500, "text/plain", "config.json not found"); return; }
  String json = f.readString();
  f.close();

  JsonDocument doc;
  if (deserializeJson(doc, json)) { _server.send(500, "text/plain", "config parse error"); return; }

  JsonArray networks = doc["wifi"]["networks"].to<JsonArray>();
  // Clear existing networks and add the new one
  networks.clear();
  JsonObject net = networks.add<JsonObject>();
  net["ssid"] = ssid;
  net["password"] = pass;

  File wf = LittleFS.open("/config.json", "w");
  if (!wf) { _server.send(500, "text/plain", "write failed"); return; }
  serializeJsonPretty(doc, wf);
  wf.close();

  char msg[96];
  snprintf(msg, sizeof(msg), "WiFi set to %s - rebooting", ssid.c_str());
  Log::info(TAG, msg);
  _server.send(200, "text/html", "<h2>Saved - rebooting...</h2><p>Connect to your WiFi network and find the device IP.</p>");
  delay(500);
  ESP.restart();
}

// Reboot the device
static void handleRestart() {
  if (!checkAuth()) return;
  _server.send(200, "text/plain", "rebooting");
  delay(500);
  ESP.restart();
}

// OTA upload - completion handler
static void handleOtaDone() {
  if (Update.hasError()) {
    char msg[128];
    snprintf(msg, sizeof(msg), "OTA failed: %s", Update.errorString());
    _server.send(500, "text/plain", msg);
    Log::error(TAG, msg);
  } else {
    _server.send(200, "text/plain", "OK - rebooting");
    Log::info(TAG, "OTA success - rebooting");
    delay(500);
    ESP.restart();
  }
}

// OTA upload - chunk handler
static void handleOtaUpload() {
  HTTPUpload& upload = _server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Log::info(TAG, "OTA upload started");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      char msg[64];
      snprintf(msg, sizeof(msg), "Update.begin failed: %s", Update.errorString());
      Log::error(TAG, msg);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      char msg[64];
      snprintf(msg, sizeof(msg), "Update.write failed: %s", Update.errorString());
      Log::error(TAG, msg);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) {
      char msg[64];
      snprintf(msg, sizeof(msg), "Update.end failed: %s", Update.errorString());
      Log::error(TAG, msg);
    } else {
      char msg[48];
      snprintf(msg, sizeof(msg), "OTA complete: %u bytes", upload.totalSize);
      Log::info(TAG, msg);
    }
  }
}

// Captive portal - redirect unknown requests to setup page
static void handleNotFound() {
  if (WiFiManager::isAPActive()) {
    _server.sendHeader("Location", "http://" + WiFi.softAPIP().toString());
    _server.send(302, "text/plain", "redirect");
  } else {
    _server.send(404, "text/plain", "not found");
  }
}

// Initialize HTTP server and register routes
void LiteServer::begin() {
  _server.on("/",           HTTP_GET,  handleRoot);
  _server.on("/api/scan",   HTTP_GET,  handleScan);
  _server.on("/api/info",   HTTP_GET,  handleInfo);
  _server.on("/api/config", HTTP_GET,  handleConfigGet);
  _server.on("/api/config", HTTP_POST, handleConfigPost);
  _server.on("/api/wifi",   HTTP_POST, handleWifi);
  _server.on("/api/restart",HTTP_POST, handleRestart);
  _server.on("/ota",        HTTP_POST, handleOtaDone, handleOtaUpload);
  _server.onNotFound(handleNotFound);

  _server.begin();
  Log::info(TAG, "LiteServer started on port 80");
}

// Process incoming HTTP requests (must be called every loop cycle)
void LiteServer::loop() {
  _server.handleClient();
}

// Module wrapper for self-registration
class LiteServerModule : public Module {
public:
  void begin() override { LiteServer::begin(); }
  void loop() override { LiteServer::loop(); }
  const char* name() override { return "LiteServer"; }
  void status(ShellOutput out) override { out("listening on :80"); }
};

MODULE_REGISTER(LiteServerModule, PRIORITY_SERVICE)

#endif // ENABLE_LITESERVER
