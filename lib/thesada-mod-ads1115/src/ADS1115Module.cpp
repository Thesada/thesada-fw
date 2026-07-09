// thesada-fw - ADS1115Module.cpp
// Reads device + channel lists from config.json at boot. Each channel specifies
// a mux pair (e.g. "A0_A1" for differential, "A0_GND" for single-ended), a PGA
// gain in volts (e.g. 1.024), and a CT clamp ratio "clamp_a_per_v" (amps per
// 1 V, default 30). Multiple ADS1115 chips on the shared I2C bus are supported
// via ads1115.devices[]; the scalar ads1115.address form is one device.
// Readings are published as a JSON array <topic_prefix>/sensor/current and as
// one combined "current" EventBus event for Lua rules.
// SPDX-License-Identifier: GPL-3.0-only

#include "ADS1115Module.h"

#ifdef ENABLE_ADS1115

#include <Config.h>
#include <EventBus.h>
#include <MQTTClient.h>
#include <Log.h>
#include <Wire.h>
#include <ModuleRegistry.h>
#include <SensorRegistry.h>

static const char* TAG = "ADS1115";

// ---------------------------------------------------------------------------

// Initialize I2C, bring up each configured ADS1115, and load channel lists.
void ADS1115Module::begin() {
  JsonObject cfg = Config::get();

  _intervalMs  = (uint32_t)(cfg["ads1115"]["interval_s"] | 60) * 1000UL;
  _lineVoltage = cfg["ads1115"]["line_voltage"] | 120.0f;

  int sda = cfg["ads1115"]["i2c_sda"] | 1;
  int scl = cfg["ads1115"]["i2c_scl"] | 2;
  Wire.begin(sda, scl);

  // Device list: ads1115.devices[{address,channels}] if present, else the
  // scalar ads1115.address + ads1115.channels form (back-compat = one device).
  JsonArray devs = cfg["ads1115"]["devices"].as<JsonArray>();
  if (!devs.isNull() && devs.size() > 0) {
    for (JsonObject d : devs) addDevice(d["address"] | 0x48, d["channels"].as<JsonArray>());
  } else {
    addDevice(cfg["ads1115"]["address"] | 0x48, cfg["ads1115"]["channels"].as<JsonArray>());
  }

  char msg[64];
  // TODO: migrate to structured logging
  snprintf(msg, sizeof(msg), "ads.ready devices=%d channels=%d",
           (int)_devices.size(), (int)channelCount());
  Log::info(TAG, msg);

  SensorRegistry::add("current", "ADS1115 CT channels (RMS current)",
    [](ShellOutput out, void* ctx) {
      static_cast<ADS1115Module*>(ctx)->sensorRead(out);
    }, this, true);
}

// ---------------------------------------------------------------------------

// Bring up one ADS1115 at the given address and load its channel list. A chip
// that fails to init is kept (channels visible in status) but skipped on read.
void ADS1115Module::addDevice(uint8_t address, JsonArray channels) {
  ADS1115Device dev;
  dev.address = address;
  dev.ads     = new Adafruit_ADS1115();
  dev.ok      = dev.ads->begin(address, &Wire);
  if (!dev.ok) {
    char msg[48];
    // TODO: migrate to structured logging
    snprintf(msg, sizeof(msg), "ads.device_missing addr=0x%02X", address);
    Log::error(TAG, msg);
  }
  loadChannels(channels, dev.channels, address);
  _devices.push_back(dev);
}

// ---------------------------------------------------------------------------

// Parse channel definitions (mux pair, gain, clamp ratio) into dst.
void ADS1115Module::loadChannels(JsonArray src, std::vector<ADS1115Channel>& dst, uint8_t address) {
  if (src.isNull()) return;

  for (JsonObject ch : src) {
    const char* cname = ch["name"] | "";
    const char* muxS  = ch["mux"]  | "A0_A1";
    float       gain  = ch["gain"] | 1.024f;
    float       clamp = ch["clamp_a_per_v"] | 30.0f;  // SCT-013-030 default
    if (clamp <= 0.0f) {                              // bad config -> default
      char w[64];
      // TODO: migrate to structured logging
      snprintf(w, sizeof(w), "ads.clamp_invalid name=%s reason=nonpositive", cname);
      Log::warn(TAG, w);
      clamp = 30.0f;
    }

    ADS1115Channel c;
    strncpy(c.name, cname, sizeof(c.name) - 1);
    c.name[sizeof(c.name) - 1] = '\0';
    c.mux        = muxFromString(muxS);
    c.gain       = gain;
    c.gainEnum   = gainFromFloat(gain);
    c.clampAPerV = clamp;
    dst.push_back(c);

    char msg[96];
    // TODO: migrate to structured logging
    snprintf(msg, sizeof(msg), "ads.channel addr=0x%02X name=%s mux=%s gain=%.3f clamp=%g",
             address, cname, muxS, gain, clamp);
    Log::info(TAG, msg);
  }
}

// ---------------------------------------------------------------------------

// Total configured channels across all devices.
size_t ADS1115Module::channelCount() const {
  size_t n = 0;
  for (auto& d : _devices) n += d.channels.size();
  return n;
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

// SensorRegistry read callback. One line per channel showing RMS voltage,
// computed current, and estimated power. Does not publish.
void ADS1115Module::sensorRead(ShellOutput out) {
  if (channelCount() == 0) { out("  no channels configured"); return; }
  char line[128];
  for (auto& dev : _devices) {
    for (auto& ch : dev.channels) {
      if (!dev.ok) {
        snprintf(line, sizeof(line), "  %-12s: device 0x%02X offline", ch.name, dev.address);
        out(line);
        continue;
      }
      float rmsV    = readRmsVoltage(*dev.ads, ch, 30);
      float current = rmsV * ch.clampAPerV;
      float power   = current * _lineVoltage;
      snprintf(line, sizeof(line),
               "  %-12s: %.4f V rms  %.2f A  %.1f W",
               ch.name, rmsV, current, power);
      out(line);
    }
  }
}

// ---------------------------------------------------------------------------

// Read every channel on every live device and publish current/power.
void ADS1115Module::readAndPublish() {
  if (channelCount() == 0) return;

  JsonDocument doc;
  JsonArray    arr = doc["channels"].to<JsonArray>();

  for (auto& dev : _devices) {
    if (!dev.ok) continue;
    for (auto& ch : dev.channels) {
      float rmsV    = readRmsVoltage(*dev.ads, ch, 30);
      // Clamp ratio is amps per 1 V output (e.g. SCT-013-030 = 30, SCT-013-005 = 5).
      float current = rmsV * ch.clampAPerV;
      float power   = current * _lineVoltage;
      int16_t raw   = dev.ads->getLastConversionResults();

      JsonObject obj    = arr.add<JsonObject>();
      obj["name"]       = ch.name;
      obj["address"]    = dev.address;  // disambiguate same-named channels across chips
      obj["raw"]        = raw;
      obj["voltage_v"]  = roundf(rmsV * 10000.0f) / 10000.0f;
      obj["current_a"]  = roundf(current * 100.0f) / 100.0f;
      obj["power_w"]    = roundf(power * 10.0f) / 10.0f;
      obj["line_v"]     = _lineVoltage;
    }
  }

  if (arr.size() == 0) return;

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

  // Combined JSON topic (for Lua EventBus, SD logging, backwards compat).
  // Dynamic buffer - 8 channels across two devices outgrow any fixed size and
  // a truncated payload would publish malformed JSON.
  char topic[64];
  snprintf(topic, sizeof(topic), "%s/sensor/current", prefix);
  String payload;
  payload.reserve(measureJson(doc) + 1);
  serializeJson(doc, payload);
  MQTTClient::publish(topic, payload.c_str());

  EventBus::publish("current", doc.as<JsonObject>());
}

// ---------------------------------------------------------------------------

// Sample N readings over ~2 full 60Hz cycles and return RMS voltage.
// ADS1115 at 860 SPS: 30 samples takes ~35ms (covers ~2 cycles at 60Hz).
float ADS1115Module::readRmsVoltage(Adafruit_ADS1115& ads, ADS1115Channel& ch, int samples) {
  ads.setGain(ch.gainEnum);
  ads.setDataRate(RATE_ADS1115_860SPS);

  float sumSq = 0.0f;
  float scale = ch.gain / 32768.0f;

  for (int i = 0; i < samples; i++) {
    int16_t raw = readChannel(ads, ch);
    float v = raw * scale;
    sumSq += v * v;
  }

  return sqrtf(sumSq / (float)samples);
}

// ---------------------------------------------------------------------------

// Adafruit ADS1X15 exposes separate named methods per mux pair.
int16_t ADS1115Module::readChannel(Adafruit_ADS1115& ads, const ADS1115Channel& ch) {
  switch (ch.mux) {
    case ADS1115Mux::DIFF_0_1: return ads.readADC_Differential_0_1();
    case ADS1115Mux::DIFF_0_3: return ads.readADC_Differential_0_3();
    case ADS1115Mux::DIFF_1_3: return ads.readADC_Differential_1_3();
    case ADS1115Mux::DIFF_2_3: return ads.readADC_Differential_2_3();
    case ADS1115Mux::SINGLE_0: return ads.readADC_SingleEnded(0);
    case ADS1115Mux::SINGLE_1: return ads.readADC_SingleEnded(1);
    case ADS1115Mux::SINGLE_2: return ads.readADC_SingleEnded(2);
    case ADS1115Mux::SINGLE_3: return ads.readADC_SingleEnded(3);
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
  return GAIN_TWOTHIRDS;                    // ±6.144 V
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
  char w[48];
  // TODO: migrate to structured logging
  snprintf(w, sizeof(w), "ads.mux_unknown value=%s", s);
  Log::warn(TAG, w);
  return ADS1115Mux::DIFF_0_1;
}

// Report ADS1115 module status from the runtime device/channel lists.
void ADS1115Module::status(ShellOutput out) {
  char addrs[48] = {0};
  for (size_t i = 0; i < _devices.size(); i++) {
    char a[10];
    snprintf(a, sizeof(a), "%s0x%02X%s", i ? "," : "",
             _devices[i].address, _devices[i].ok ? "" : "!");
    strncat(addrs, a, sizeof(addrs) - strlen(addrs) - 1);
  }
  char line[96];
  snprintf(line, sizeof(line), "%d channel(s) on %d device(s) [%s]  interval=%ds",
           (int)channelCount(), (int)_devices.size(), addrs, (int)(_intervalMs / 1000));
  out(line);
}

MODULE_REGISTER(ADS1115Module, PRIORITY_SENSOR)

#endif // ENABLE_ADS1115
