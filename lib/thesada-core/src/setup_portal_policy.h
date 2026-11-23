// thesada-fw - setup_portal_policy.h
// When the fallback-AP setup page may exist, and whether its fields are usable.
// Host-unit-testable. See docs/invariants.md.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "enroll_policy.h"
#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

// The page is up while the setup AP is up and no station is saved, or a result is held.
// in: ap up, saved station networks, result held. out: true to serve it.
inline bool setupPortalOpen(bool apActive, size_t stationNetworks, bool holdingResult) {
  return apActive && (stationNetworks == 0 || holdingResult);
}

// SSID the radio will store: 1..32 bytes, no controls. Quotes and non-ASCII
// stay; the page never writes the name back out. in: ssid. out: usable.
inline bool setupSsidUsable(const char* ssid) {
  if (!ssid || !ssid[0]) return false;
  size_t n = 0;
  for (; ssid[n]; n++) {
    unsigned char c = (unsigned char)ssid[n];
    if (c < 0x20 || c == 0x7f) return false;
  }
  return n <= 32;
}

// WPA2 passphrase: 8..63 printable ASCII, no controls.
// in: pass. out: true when the radio can use it.
inline bool setupPassUsable(const char* pass) {
  if (!pass) return false;
  size_t n = 0;
  for (; pass[n]; n++) {
    unsigned char c = (unsigned char)pass[n];
    if (c < 0x20 || c >= 0x7f) return false;
  }
  return n >= 8 && n <= 63;
}

// The three fields the page posts. The URL rule is the enrollment one.
// in: ssid, pass, url. out: true when all three can be stored.
inline bool setupFormUsable(const char* ssid, const char* pass, const char* url) {
  return setupSsidUsable(ssid) && setupPassUsable(pass) && enrollUrlUsable(url);
}

// The setup form into a staged config: the network list becomes this one SSID,
// enroll.url the base. A wifi, networks or enroll that is not the right type is
// replaced. in: staged root, ssid, enroll base. out: false when a write did not land.
inline bool setupFormToConfig(JsonObject cfg, const char* ssid, const char* enrollBase) {
  if (cfg.isNull()) return false;
  JsonObject wifi = cfg["wifi"].is<JsonObject>() ? cfg["wifi"].as<JsonObject>() : cfg["wifi"].to<JsonObject>();
  if (wifi.isNull()) return false;
  JsonArray nets = wifi["networks"].to<JsonArray>();
  if (nets.isNull()) return false;
  JsonObject net = nets.add<JsonObject>();
  if (net.isNull() || !net["ssid"].set(ssid)) return false;
  JsonObject enroll = cfg["enroll"].is<JsonObject>() ? cfg["enroll"].as<JsonObject>() : cfg["enroll"].to<JsonObject>();
  return !enroll.isNull() && enroll["url"].set(enrollBase);
}
