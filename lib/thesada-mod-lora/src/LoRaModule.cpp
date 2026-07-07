// thesada-fw - LoRaModule.cpp
// Glue between thesada-core and the SX1262 (via LoRaRadio): config, shell
// commands, and RX publish. No RadioLib include here - see LoRaRadio.h.
// SPDX-License-Identifier: GPL-3.0-only
#include "LoRaModule.h"

#ifdef ENABLE_LORA

#include <Config.h>
#include <EventBus.h>
#include <MQTTClient.h>
#include <Log.h>
#include <lora_dedup_policy.h>
#include <ModuleRegistry.h>
#include <Shell.h>
#include <ArduinoJson.h>
#include <meshtastic_radio.h>
#include <esp_random.h>

static const char* TAG = "LoRa";

// base-lora-carrier pin map (schematic + fab netlist, authoritative).
#define LORA_NSS  17
#define LORA_SCK  12
#define LORA_MISO 13
#define LORA_MOSI 11
#define LORA_NRST 47
#define LORA_BUSY 48
#define LORA_RXEN 14
#define LORA_TXEN 16
#define LORA_TCXO_V 1.6f

// Read config, bring up the radio, register shell commands.
void LoRaModule::begin() {
  JsonObject cfg = Config::get();
  _freq     = cfg["lora"]["freq_mhz"]     | 915.0f;
  _bw       = cfg["lora"]["bw_khz"]       | 125.0f;
  _sf       = cfg["lora"]["sf"]           | 9;
  _cr       = cfg["lora"]["cr"]           | 7;
  _power    = cfg["lora"]["tx_power_dbm"] | 14;
  _preamble = cfg["lora"]["preamble"]     | 8;
  _syncWord = cfg["lora"]["sync_word"]    | 0x12;
  bool listenBoot = cfg["lora"]["listen_on_boot"] | true;

  // Mode: "thesada" (default, raw #505 path) or "meshtastic" (derived PHY +
  // frame interop). The v0 bool `lora.meshtastic: true` stays as an alias
  // for one release.
  const char* mode = cfg["lora"]["mode"] | "";
  JsonVariant meshCfg = cfg["lora"]["meshtastic"];
  if (*mode && strcmp(mode, "thesada") != 0 && strcmp(mode, "meshtastic") != 0) {
    char m[64];
    snprintf(m, sizeof(m), "lora.mode_invalid mode=%s", mode);
    Log::error(TAG, m);
    return;  // fail closed - a mistyped mode must not come up off-mesh
  }
  _meshtastic = strcmp(mode, "meshtastic") == 0 ||
                (!*mode && meshCfg.is<bool>() && meshCfg.as<bool>());
  _nodeNum = cfg["lora"]["node_num"] | (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF);
  if (!_nodeNum) _nodeNum = 1;
  if (_meshtastic && !meshConfig()) return;  // logged inside; fail closed

  int st = _radio.begin(LORA_NSS, LORA_SCK, LORA_MISO, LORA_MOSI,
                        LORA_NRST, LORA_BUSY, LORA_RXEN, LORA_TXEN,
                        _freq, _bw, _sf, _cr, _syncWord, _power, _preamble, LORA_TCXO_V);
  if (st != 0) {
    char m[48];
    snprintf(m, sizeof(m), "lora.init_failed code=%d", st);
    Log::error(TAG, m);
    return;
  }
  _ok = true;
  if (listenBoot) setListening(true);

  Shell::registerCommand("lora.send",
    "Transmit a text packet (lora.send <text>)",
    [this](int argc, char** argv, ShellOutput out) {
      if (argc < 2) { out("usage: lora.send <text>"); return; }
      String msg;
      for (int i = 1; i < argc; i++) { if (i > 1) msg += " "; msg += argv[i]; }
      out(transmit(msg.c_str()) ? "TX_DONE" : "TX failed/timeout");
    });

  Shell::registerCommand("lora.status",
    "Radio config + last RX stats",
    [this](int, char**, ShellOutput out) { status(out); });

  Shell::registerCommand("lora.listen",
    "Toggle continuous RX (lora.listen on|off)",
    [this](int argc, char** argv, ShellOutput out) {
      if (argc >= 2) setListening(strcmp(argv[1], "on") == 0);
      out(_listening ? "listening" : "idle");
    });

  Shell::registerCommand("lora.rssi",
    "Instantaneous RSSI in dBm (band noise floor / activity)",
    [this](int, char**, ShellOutput out) {
      if (!_ok) { out("radio not initialized"); return; }
      char line[32];
      snprintf(line, sizeof(line), "rssi=%.1f dBm", _radio.instantRssi());
      out(line);
    });

  char msg[96];
  snprintf(msg, sizeof(msg), "lora.ready freq=%.3f sf=%d bw=%.0f power=%d listen=%d mode=%s",
           _freq, _sf, _bw, _power, _listening, _meshtastic ? "meshtastic" : "thesada");
  Log::info(TAG, msg);
}

// Derive the Meshtastic PHY + channel crypto from config. False (with a log)
// on any invalid value - fail closed rather than transmit off-mesh.
bool LoRaModule::meshConfig() {
  JsonVariant mv = Config::get()["lora"]["meshtastic"];
  const char* region  = "US";
  const char* preset  = "LongFast";
  const char* channel = "";
  const char* pskB64  = "";
  if (mv.is<JsonObject>()) {
    region  = mv["region"]  | "US";
    preset  = mv["preset"]  | "LongFast";
    channel = mv["channel"] | "";
    pskB64  = mv["psk"]     | "";
  }

  const mesh::Preset* p = mesh::presetFind(preset);
  const mesh::Region* r = mesh::regionFind(region);
  if (!p || !r) {
    char m[96];
    snprintf(m, sizeof(m), "lora.mesh_config_invalid preset=%s region=%s", preset, region);
    Log::error(TAG, m);
    return false;
  }
  if (!*channel) channel = p->name;  // Meshtastic default-channel rule

  uint8_t psk[32];
  size_t pskLen = 0;
  if (*pskB64) {
    pskLen = mesh::b64Decode(pskB64, psk, sizeof(psk));
    if (pskLen == (size_t)-1) {
      Log::error(TAG, "lora.mesh_psk_invalid err=base64");
      return false;
    }
  }
  if (!mesh::pskExpand(psk, pskLen, _chan.key, _chan.keyLen)) {
    char m[48];
    snprintf(m, sizeof(m), "lora.mesh_psk_invalid len=%u", (unsigned)pskLen);
    Log::error(TAG, m);
    return false;
  }
  _chan.hash = mesh::channelHash(channel, _chan.key, _chan.keyLen);

  float f = mesh::slotFreqMhz(*r, p->bwKhz, channel, _slot);
  if (f <= 0.0f) {
    char m[96];
    snprintf(m, sizeof(m), "lora.mesh_config_invalid preset=%s too wide for region=%s", preset, region);
    Log::error(TAG, m);
    return false;
  }
  _freq = f; _bw = p->bwKhz; _sf = p->sf; _cr = p->cr;
  _syncWord = mesh::SYNC;
  _preamble = mesh::PREAMBLE;
  snprintf(_chanName, sizeof(_chanName), "%s", channel);

  char m[112];
  snprintf(m, sizeof(m), "lora.mesh_channel name=%s slot=%u freq=%.3f key=%ubit hash=0x%02x",
           _chanName, _slot + 1, _freq, (unsigned)(_chan.keyLen * 8), _chan.hash);
  Log::info(TAG, m);
  return true;
}

// Poll-mode RX pump (DIO1 NC). Publishes clean packets, logs dropped ones.
void LoRaModule::loop() {
  if (!_ok || !_listening) return;
  LoRaRx rx;
  if (!_radio.poll(rx)) return;

  _rxCount++;
  _lastRssi = rx.rssi;
  _lastSnr  = rx.snr;
  if (rx.received && !rx.crcErr) {
    publishRx(rx);
  } else {
    char m[64];
    snprintf(m, sizeof(m), "lora.rx_dropped crc=%d rssi=%.1f", rx.crcErr, rx.rssi);
    Log::warn(TAG, m);
  }
}

// Transmit, then resume RX if we were listening (TX leaves the radio in standby).
bool LoRaModule::transmit(const char* msg) {
  if (!_ok) return false;
  bool wasListening = _listening;
  bool done;
  if (_meshtastic) {
    uint8_t buf[256];
    uint32_t pid = esp_random();
    if (!pid) pid = 1;
    size_t n = mesh::buildText(_chan, msg, _nodeNum, pid, buf, sizeof(buf));
    done = n && _radio.transmit(buf, n, 4000);
  } else {
    done = _radio.transmit(msg, 4000);
  }
  if (wasListening) _radio.startReceive();

  char m[64];
  snprintf(m, sizeof(m), "lora.tx done=%d len=%d", done, (int)strlen(msg));
  Log::info(TAG, m);
  return done;
}

void LoRaModule::setListening(bool on) {
  if (!_ok) return;
  _listening = on;
  if (on) _radio.startReceive(); else _radio.standby();
}

// Publish a received packet to MQTT (<prefix>/lora/rx) and the EventBus.
void LoRaModule::publishRx(const LoRaRx& rx) {
  String text;
  uint32_t from = 0;
  if (_meshtastic) {
    // Ignore mesh traffic that isn't a text message on our channel, but
    // name a foreign portnum at debug so bench diagnosis is not blind.
    uint32_t port = 0;
    uint32_t pid = 0;
    mesh::Parse pr = mesh::parseText(_chan, rx.data, rx.len, text, from, port, pid);
    if (pr != mesh::Parse::Ok) {
      // Never drop silently - undecodable traffic is invisible otherwise
      // (cost a bench session to a quiet console).
      char m[56];
      if (pr == mesh::Parse::ForeignPort) {
        snprintf(m, sizeof(m), "lora.rx_foreign_port port=%u", (unsigned)port);
      } else {
        snprintf(m, sizeof(m), "lora.rx_undecoded %s len=%u",
                 pr == mesh::Parse::NotOurs ? "other_channel" : "malformed",
                 (unsigned)rx.len);
      }
      Log::debug(TAG, m);
      return;
    }
    // Broadcast retransmits reuse (src, packetId) - drop repeats before
    // they republish to MQTT. Debug-visible, counted for module.status.
    static mesh::DedupRing<16> rxSeen;
    if (rxSeen.seenAndRecord(from, pid)) {
      _rxDupCount++;
      char m[48];
      snprintf(m, sizeof(m), "lora.rx_dup from=%08x id=%08x", (unsigned)from, (unsigned)pid);
      Log::debug(TAG, m);
      return;
    }
  } else {
    // thesada mode is plain text - stop at the first NUL, as the old
    // String-based read did.
    for (size_t k = 0; k < rx.len && rx.data[k]; k++) text += (char)rx.data[k];
  }

  JsonDocument doc;
  doc["data"]  = text;
  if (_meshtastic) {
    char nid[12];
    snprintf(nid, sizeof(nid), "!%08x", (unsigned)from);
    doc["from"] = nid;
  }
  doc["rssi"]  = rx.rssi;
  doc["snr"]   = rx.snr;
  doc["count"] = _rxCount;

  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  char topic[64];
  snprintf(topic, sizeof(topic), "%s/lora/rx", prefix);

  String payload;
  payload.reserve(measureJson(doc) + 1);
  serializeJson(doc, payload);
  MQTTClient::publish(topic, payload.c_str());
  EventBus::publish("lora", doc.as<JsonObject>());

  char m[80];
  snprintf(m, sizeof(m), "lora.rx len=%d rssi=%.1f snr=%.1f",
           (int)text.length(), rx.rssi, rx.snr);
  Log::info(TAG, m);
}

void LoRaModule::status(ShellOutput out) {
  if (!_ok) { out("  radio not initialized"); return; }
  char line[160];
  snprintf(line, sizeof(line),
           "mode=%s freq=%.3fMHz sf=%d bw=%.0fkHz power=%ddBm listening=%s rx=%lu rx_dup=%lu last_rssi=%.1f last_snr=%.1f",
           _meshtastic ? "meshtastic" : "thesada",
           _freq, _sf, _bw, _power, _listening ? "yes" : "no",
           (unsigned long)_rxCount, (unsigned long)_rxDupCount, _lastRssi, _lastSnr);
  out(line);
  if (_meshtastic) {
    snprintf(line, sizeof(line), "channel=%s slot=%u key=%ubit hash=0x%02x",
             _chanName, _slot + 1, (unsigned)(_chan.keyLen * 8), _chan.hash);
    out(line);
  }
}

MODULE_REGISTER(LoRaModule, PRIORITY_SENSOR)

#endif // ENABLE_LORA
