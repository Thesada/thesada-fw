// thesada-fw - LiteServer.cpp
// Lightweight HTTP server using Arduino built-in WebServer (blocking, single-client).
// Provides push OTA, config editor, and WiFi AP provisioning for heap-constrained boards.
// SPDX-License-Identifier: GPL-3.0-only

#include <thesada_config.h>
#ifdef ENABLE_LITESERVER

#include "LiteServer.h"
#include <Config.h>
#include <Secret.h>
#include <Log.h>
#include <WiFiManager.h>
#include <ModuleRegistry.h>
#include <Module.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <MQTTClient.h>
#include "liteserver.html.h"

static const char* TAG = "LiteServer";
static WebServer _server(80);

// Check Basic Auth - skip in AP mode (no credentials configured yet).
// No response side effects, so it is safe inside upload callbacks.
static bool authOk() {
  if (WiFiManager::isAPActive()) return true;
  JsonObject cfg = Config::get();
  const char* user = cfg["web"]["user"]     | "";
  char passBuf[Secret::MAX_LEN];
  const char* pass = Secret::resolve("web_password", cfg["web"]["password"] | "",
                                     passBuf, sizeof(passBuf));
  if (strlen(user) == 0 && strlen(pass) == 0) {
    static bool _warnedOnce = false;
    if (!_warnedOnce) { Log::kvfw(TAG, "web.admin_unprotected reason=no_credentials"); _warnedOnce = true; }
    return true;
  }
  return _server.authenticate(user, pass);
}

// authOk plus the 401 challenge - use in request handlers
static bool checkAuth() {
  if (!authOk()) {
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

// Write body to config.json via tmp + rename so a short write (fs full)
// never destroys the last good config - same policy as Config::save().
// out: true on success; on failure the tmp is removed and the original
// file is untouched.
static bool atomicConfigWrite(const String& body) {
  File f = LittleFS.open("/config.json.tmp", "w");
  if (!f) return false;
  size_t written = f.print(body);
  f.close();
  if (written != body.length() ||
      !LittleFS.rename("/config.json.tmp", "/config.json")) {
    LittleFS.remove("/config.json.tmp");
    return false;
  }
  return true;
}

// Save posted JSON body to config.json
static void handleConfigPost() {
  if (!checkAuth()) return;
  if (!_server.hasArg("plain")) { _server.send(400, "text/plain", "empty body"); return; }
  String body = _server.arg("plain");
  // Validate JSON before saving
  JsonDocument doc;
  if (deserializeJson(doc, body)) { _server.send(400, "text/plain", "invalid JSON"); return; }
  if (!atomicConfigWrite(body)) { _server.send(500, "text/plain", "write failed"); return; }
  Config::load();
  _server.send(200, "text/plain", "saved");
  Log::info(TAG, "web.config_saved via=liteserver");
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

  String out;
  serializeJsonPretty(doc, out);
  if (out.isEmpty() || !atomicConfigWrite(out)) { _server.send(500, "text/plain", "write failed"); return; }

  Log::kvf(TAG, "web.wifi_config_saved ssid=%s action=reboot", ssid.c_str());
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

// True while the in-flight OTA upload passed auth at UPLOAD_FILE_START
static bool _otaAuthorized = false;

// OTA upload - completion handler
static void handleOtaDone() {
  if (!checkAuth()) return;
  if (!_otaAuthorized) { _server.send(400, "text/plain", "no firmware received"); return; }
  _otaAuthorized = false;
  if (Update.hasError()) {
    char msg[128];
    snprintf(msg, sizeof(msg), "OTA failed: %s", Update.errorString());
    _server.send(500, "text/plain", msg);
    Log::kvfe(TAG, "web.ota_failed err=\"%s\"", Update.errorString());
  } else {
    _server.send(200, "text/plain", "OK - rebooting");
    Log::info(TAG, "web.ota_done action=reboot");
    delay(500);
    ESP.restart();
  }
}

// OTA upload - chunk handler
static void handleOtaUpload() {
  HTTPUpload& upload = _server.upload();
  // Upload chunks arrive before handleOtaDone runs its auth check, so gate
  // here too - unauthorized bodies must never reach Update.
  if (upload.status == UPLOAD_FILE_START) {
    _otaAuthorized = authOk();
    if (!_otaAuthorized) { Log::warn(TAG, "web.ota_upload_refused reason=unauthorized"); return; }
    Log::info(TAG, "web.ota_upload_start");
    // Free MQTT TLS buffers so Update.begin() can malloc on low-heap boards
    if (MQTTClient::connected() && ESP.getMaxAllocHeap() < 40000) {
      Log::info(TAG, "web.mqtt_disconnect reason=heap_low_for_ota");
      MQTTClient::_client.disconnect();
      MQTTClient::_wifiClient.stop();
      MQTTClient::_connectedSinceMs = 0;
      delay(100);
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Log::kvfe(TAG, "web.ota_begin_failed err=\"%s\"", Update.errorString());
    }
  } else if (!_otaAuthorized) {
    return;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    esp_task_wdt_reset();
    yield();
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Log::kvfe(TAG, "web.ota_write_failed err=\"%s\"", Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) {
      Log::kvfe(TAG, "web.ota_end_failed err=\"%s\"", Update.errorString());
    } else {
      Log::kvf(TAG, "web.ota_upload_done bytes=%u", upload.totalSize);
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
  Log::info(TAG, "web.server_started port=80 variant=lite");
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
  const char* configKey() override { return "web"; }
  void status(ShellOutput out) override { out("listening on :80"); }
};

MODULE_REGISTER(LiteServerModule, PRIORITY_SERVICE)

#endif // ENABLE_LITESERVER
