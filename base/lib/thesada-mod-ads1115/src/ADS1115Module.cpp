// thesada-fw - ADS1115Module.cpp
// Reads the channel list from config.json at boot. Each channel specifies a
// mux pair (e.g. "A0_A1" for differential, "A0_GND" for single-ended) and a
// PGA gain value in volts (e.g. 1.024). Readings are published as a JSON array:
//   <topic_prefix>/sensor/current
// and as individual "current" EventBus events per channel for Lua rules.
// SPDX-License-Identifier: GPL-3.0-only

#include "ADS1115Module.h"

#ifdef ENABLE_ADS1115

#include <Config.h>
#include <EventBus.h>
#include <MQTTClient.h>
#include <Log.h>
#include <Wire.h>
#include <ModuleRegistry.h>

static const char* TAG = "ADS1115";

// ---------------------------------------------------------------------------

// Initialize I2C, configure ADS1115, and load channel definitions from config
void ADS1115Module::begin() {
  JsonObject cfg = Config::get();

  _intervalMs  = (uint32_t)(cfg["ads1115"]["interval_s"] | 60) * 1000UL;
  _lineVoltage = cfg["ads1115"]["line_voltage"] | 120.0f;

  // I2C pins and address are configurable; fall back to board defaults.
  int sda  = cfg["ads1115"]["i2c_sda"] | 1;
  int scl  = cfg["ads1115"]["i2c_scl"] | 2;
  int addr = cfg["ads1115"]["address"]  | 0x48;

  Wire.begin(sda, scl);

  if (!_ads.begin(addr, &Wire)) {
    Log::error(TAG, "ADS1115 not found - check wiring and I2C address");
    return;
  }

  loadChannels();

  char msg[64];
  snprintf(msg, sizeof(msg), "Ready - %d channel(s) at I2C 0x%02X (SDA=%d SCL=%d)",
           (int)_channels.size(), addr, sda, scl);
  Log::info(TAG, msg);
}

// ---------------------------------------------------------------------------

// Parse channel definitions (mux pair, gain) from config.json
void ADS1115Module::loadChannels() {
  JsonObject cfg      = Config::get();
  JsonArray  channels = cfg["ads1115"]["channels"].as<JsonArray>();
  if (channels.isNull()) return;

  for (JsonObject ch : channels) {
    const char* cname = ch["name"] | "";
    const char* muxS  = ch["mux"]  | "A0_A1";
    float       gain  = ch["gain"] | 1.024f;

    ADS1115Channel c;
    strncpy(c.name, cname, sizeof(c.name) - 1);
    c.name[sizeof(c.name) - 1] = '\0';
    c.mux      = muxFromString(muxS);
    c.gain     = gain;
    c.gainEnum = gainFromFloat(gain);
    _channels.push_back(c);

    char msg[64];
    snprintf(msg, sizeof(msg), "  '%s'  mux=%s  gain=%.3fV", cname, muxS, gain);
    Log::info(TAG, msg);
  }
}

// ---------------------------------------------------------------------------

// Trigger a read-and-publish cycle at the configured interval
void ADS1115Module::loop() {
  uint32_t now = millis();
  if (now - _lastRead >= _intervalMs) {
    _lastRead = now;
    readAndPublish();
  }
}

// ---------------------------------------------------------------------------

// Read all channels and publish current/power via MQTT and EventBus
void ADS1115Module::readAndPublish() {
  if (_channels.empty()) return;

  JsonDocument doc;
  JsonArray    arr = doc["channels"].to<JsonArray>();

  for (auto& ch : _channels) {
    float rmsV    = readRmsVoltage(ch, 30);
    // SCT-013-030: 30A input produces 1V output, so current_A = voltage_rms * 30
    float current = rmsV * 30.0f;
    float power   = current * _lineVoltage;
    int16_t raw   = _ads.getLastConversionResults();

    JsonObject obj    = arr.add<JsonObject>();
    obj["name"]       = ch.name;
    obj["raw"]        = raw;
    obj["voltage_v"]  = roundf(rmsV * 10000.0f) / 10000.0f;
    obj["current_a"]  = roundf(current * 100.0f) / 100.0f;
    obj["power_w"]    = roundf(power * 10.0f) / 10.0f;
    obj["line_v"]     = _lineVoltage;
  }

  JsonObject cfg = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";

  // Per-sensor topics for HA discovery
  for (JsonObject ch : arr) {
    const char* cname = ch["name"] | "unknown";
    char slug[32];
    strncpy(slug, cname, sizeof(slug) - 1);
    slug[sizeof(slug) - 1] = '\0';
    for (char* p = slug; *p; p++) { if (*p == ' ') *p = '_'; *p = tolower(*p); }

    char perTopic[96], val[16];
    snprintf(perTopic, sizeof(perTopic), "%s/sensor/current/%s", prefix, slug);
    snprintf(val, sizeof(val), "%.2f", (float)(ch["current_a"] | 0.0f));
    MQTTClient::publish(perTopic, val);

    snprintf(perTopic, sizeof(perTopic), "%s/sensor/power/%s", prefix, slug);
    snprintf(val, sizeof(val), "%.1f", (float)(ch["power_w"] | 0.0f));
    MQTTClient::publish(perTopic, val);
  }

  // Combined JSON topic (for Lua EventBus, SD logging, backwards compat)
  char topic[64];
  snprintf(topic, sizeof(topic), "%s/sensor/current", prefix);
  char payload[512];
  serializeJson(doc, payload, sizeof(payload));
  MQTTClient::publish(topic, payload);

  EventBus::publish("current", doc.as<JsonObject>());
}

// ---------------------------------------------------------------------------

// Sample N readings over ~2 full 60Hz cycles and return RMS voltage.
// ADS1115 at 860 SPS: 30 samples takes ~35ms (covers ~2 cycles at 60Hz).
float ADS1115Module::readRmsVoltage(ADS1115Channel& ch, int samples) {
  _ads.setGain(ch.gainEnum);
  _ads.setDataRate(RATE_ADS1115_860SPS);

  float sumSq = 0.0f;
  float scale = ch.gain / 32768.0f;

  for (int i = 0; i < samples; i++) {
    int16_t raw = readChannel(ch);
    float v = raw * scale;
    sumSq += v * v;
  }

  return sqrtf(sumSq / (float)samples);
}

// ---------------------------------------------------------------------------

// Adafruit ADS1X15 exposes separate named methods per mux pair.
int16_t ADS1115Module::readChannel(const ADS1115Channel& ch) {
  switch (ch.mux) {
    case ADS1115Mux::DIFF_0_1: return _ads.readADC_Differential_0_1();
    case ADS1115Mux::DIFF_0_3: return _ads.readADC_Differential_0_3();
    case ADS1115Mux::DIFF_1_3: return _ads.readADC_Differential_1_3();
    case ADS1115Mux::DIFF_2_3: return _ads.readADC_Differential_2_3();
    case ADS1115Mux::SINGLE_0: return _ads.readADC_SingleEnded(0);
    case ADS1115Mux::SINGLE_1: return _ads.readADC_SingleEnded(1);
    case ADS1115Mux::SINGLE_2: return _ads.readADC_SingleEnded(2);
    case ADS1115Mux::SINGLE_3: return _ads.readADC_SingleEnded(3);
    default:                   return 0;
  }
}

// Map a gain voltage to the nearest ADS1115 PGA enum value
adsGain_t ADS1115Module::gainFromFloat(float g) {
  if (g <= 0.256f) return GAIN_SIXTEEN;    // ±0.256 V
  if (g <= 0.512f) return GAIN_EIGHT;      // ±0.512 V
  if (g <= 1.024f) return GAIN_FOUR;       // ±1.024 V
  if (g <= 2.048f) return GAIN_TWO;        // ±2.048 V
  if (g <= 4.096f) return GAIN_ONE;        // ±4.096 V
  return GAIN_TWOTHIRDS;                   // ±6.144 V
}

// Convert a mux config string like "A0_A1" to the corresponding enum
ADS1115Mux ADS1115Module::muxFromString(const char* s) {
  if (strcmp(s, "A0_A1") == 0) return ADS1115Mux::DIFF_0_1;
  if (strcmp(s, "A0_A3") == 0) return ADS1115Mux::DIFF_0_3;
  if (strcmp(s, "A1_A3") == 0) return ADS1115Mux::DIFF_1_3;
  if (strcmp(s, "A2_A3") == 0) return ADS1115Mux::DIFF_2_3;
  if (strcmp(s, "A0_GND") == 0) return ADS1115Mux::SINGLE_0;
  if (strcmp(s, "A1_GND") == 0) return ADS1115Mux::SINGLE_1;
  if (strcmp(s, "A2_GND") == 0) return ADS1115Mux::SINGLE_2;
  if (strcmp(s, "A3_GND") == 0) return ADS1115Mux::SINGLE_3;
  Log::warn(TAG, "Unknown mux string - defaulting to A0_A1");
  return ADS1115Mux::DIFF_0_1;
}

// Report ADS1115 module status
void ADS1115Module::status(ShellOutput out) {
  JsonObject cfg = Config::get();
  int iv = cfg["ads1115"]["interval_s"] | 0;
  JsonArray ch = cfg["ads1115"]["channels"].as<JsonArray>();
  int cnt = ch.isNull() ? 0 : ch.size();
  char line[96];
  snprintf(line, sizeof(line), "%d channel(s)  interval=%ds", cnt, iv);
  out(line);
}

MODULE_REGISTER(ADS1115Module, PRIORITY_SENSOR)

#endif // ENABLE_ADS1115
