// thesada-fw - SetupPortal.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "SetupPortal.h"
#include "Enroll.h"
#include <thesada_config.h>
#ifdef ENABLE_WEBSERVER
#include "setup_portal_policy.h"
#include "Config.h"
#include "Identity.h"
#include "Secret.h"
#include "WiFiManager.h"
#include "Log.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <mbedtls/platform_util.h>
#include <stdio.h>
#include <string.h>

static const char* TAG = "Setup";

static constexpr size_t SSID_CAP = 33;
static constexpr size_t PASS_CAP = 64;
static constexpr size_t LINK_CAP = 320;

static volatile bool _open = false;
static volatile bool _pending = false;
static volatile bool _resultReady = false;
static char _ssid[SSID_CAP];
static char _pass[PASS_CAP];
static char _url[ENROLL_URL_CAP];
static char _link[LINK_CAP];
static const char* _error = "";

// Saved station networks. in: none. out: count, 0 when the array is absent.
static size_t stationNetworks() {
  JsonArray nets = Config::get()["wifi"]["networks"].as<JsonArray>();
  if (nets.isNull()) return 0;
  return nets.size();
}

// Copy src into dst, always NUL-terminated. in: dst, cap, src. out: none.
static void copyField(char* dst, size_t cap, const char* src) {
  if (!src) src = "";
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

// Store the posted network and enroll URL, then build the claim link.
// in: pending fields. out: true when the page must stay up.
static bool applyForm() {
  _link[0] = '\0';
  _error = "";
  // New passphrase before the network. An existing one is replaced only after save.
  char field[sizeof("wifi.password:") + SSID_CAP];
  int wrote = snprintf(field, sizeof(field), "wifi.password:%s", _ssid);
  bool fieldOk = wrote > 0 && (size_t)wrote < sizeof(field);
  bool had = fieldOk && Secret::has(field);
  if (!fieldOk || (!had && !Secret::set(field, _pass))) {
    mbedtls_platform_zeroize(field, sizeof(field));
    _error = "could not store the passphrase";
    return true;
  }
  char base[ENROLL_URL_CAP];
  size_t n = enrollBaseLen(_url);
  if (n >= sizeof(base)) n = sizeof(base) - 1;
  memcpy(base, _url, n);
  base[n] = '\0';
  JsonObject cfg = Config::get();
  JsonArray nets = cfg["wifi"]["networks"].is<JsonArray>()
                       ? cfg["wifi"]["networks"].as<JsonArray>()
                       : cfg["wifi"]["networks"].to<JsonArray>();
  nets.clear();
  JsonObject net = nets.add<JsonObject>();
  net["ssid"] = String(_ssid);
  JsonObject enroll = cfg["enroll"].is<JsonObject>() ? cfg["enroll"].as<JsonObject>()
                                                      : cfg["enroll"].to<JsonObject>();
  enroll["url"] = String(base);
  if (!Config::save()) {
    Config::load();
    if (!had) Secret::clear(field);
    mbedtls_platform_zeroize(field, sizeof(field));
    _error = "could not save the network";
    return true;
  }
  if (had && !Secret::set(field, _pass)) {
    mbedtls_platform_zeroize(field, sizeof(field));
    _error = "could not store the passphrase";
    return true;
  }
  mbedtls_platform_zeroize(field, sizeof(field));
  char code[Secret::MAX_LEN] = {};
  bool have = enrollLoadClaimCode(code, sizeof(code));
  const char* id = Identity::deviceId();
  if (!have) {
    _error = "network saved; no claim code is seeded";
  } else if (!id || !identityDeviceIdValid(id)) {
    _error = "network saved; claim link could not be built";
  } else if (!enrollClaimLink(_link, sizeof(_link), base, id, code)) {
    _error = "claim link did not fit";
  }
  mbedtls_platform_zeroize(code, sizeof(code));
  return true;
}

void SetupPortal::loop() {
  if (_pending) {
    bool hold = applyForm();
    mbedtls_platform_zeroize(_pass, sizeof(_pass));
    _pending = false;
    _resultReady = hold;
    Log::kvf(TAG, "setup.applied ok=%d", _link[0] != '\0');
  }
  _open = setupPortalOpen(WiFiManager::isAPActive(), stationNetworks(), _resultReady);
}

bool SetupPortal::open() { return _open; }

bool SetupPortal::busy() { return _pending; }

bool SetupPortal::submit(const char* ssid, const char* pass, const char* url) {
  if (!_open || _pending) return false;
  if (_resultReady && _link[0]) return false;
  if (!setupFormUsable(ssid, pass, url)) return false;
  copyField(_ssid, sizeof(_ssid), ssid);
  copyField(_pass, sizeof(_pass), pass);
  copyField(_url, sizeof(_url), url);
  _link[0] = '\0';
  _error = "";
  _pending = true;
  return true;
}

bool SetupPortal::hasResult() { return _resultReady; }
const char* SetupPortal::claimLink() { return _link; }
const char* SetupPortal::resultError() { return _error; }

#endif
