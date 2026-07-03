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
#include <ModuleRegistry.h>
#include <Shell.h>
#include <ArduinoJson.h>
#include "MeshtasticFrame.h"
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

  // Meshtastic mode: override PHY to LongFast/US so the radio is on-mesh.
  _meshtastic = cfg["lora"]["meshtastic"] | false;
  _nodeNum    = cfg["lora"]["node_num"]   | (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF);
  if (!_nodeNum) _nodeNum = 1;
  if (_meshtastic) {
    _freq = mesh::FREQ_MHZ; _bw = mesh::BW_KHZ; _sf = mesh::SF; _cr = mesh::CR;
    _syncWord = mesh::SYNC; _preamble = mesh::PREAMBLE;
  }

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
  snprintf(msg, sizeof(msg), "lora.ready freq=%.1f sf=%d bw=%.0f power=%d listen=%d mode=%s",
           _freq, _sf, _bw, _power, _listening, _meshtastic ? "meshtastic" : "thesada");
  Log::info(TAG, msg);
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
    size_t n = mesh::buildText(msg, _nodeNum, pid, buf, sizeof(buf));
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
    // Ignore mesh traffic that isn't a text message on our channel.
    if (!mesh::parseText((const uint8_t*)rx.data.c_str(), rx.data.length(), text, from)) return;
  } else {
    text = rx.data;
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
  char line[144];
  snprintf(line, sizeof(line),
           "freq=%.1fMHz sf=%d bw=%.0fkHz power=%ddBm listening=%s rx=%lu last_rssi=%.1f last_snr=%.1f",
           _freq, _sf, _bw, _power, _listening ? "yes" : "no",
           (unsigned long)_rxCount, _lastRssi, _lastSnr);
  out(line);
}

MODULE_REGISTER(LoRaModule, PRIORITY_SENSOR)

#endif // ENABLE_LORA
