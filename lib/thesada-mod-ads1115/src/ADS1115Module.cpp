// thesada-fw - ADS1115Module.cpp
// Reads device + channel lists from config.json at boot. Each channel specifies
// a mux pair (e.g. "A0_A1" for differential, "A0_GND" for single-ended), a PGA
// gain in volts rounded up to an ADS1115 range, and a CT clamp ratio
// "clamp_a_per_v" (amps per 1 V, default 30). Readings are AC RMS, so a
// single-ended CT needs a mid-rail bias. "dc": true reads the mean instead, for
// a DC sensor. Multiple chips go in ads1115.devices[], the scalar
// ads1115.address form is one device.
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
#include <ads_sample_policy.h>

static const char* TAG = "ADS1115";

static const int RMS_SAMPLES = 30;

// adsGain_t per ADS_PGA_FS_V index.
static const adsGain_t GAIN_BY_PGA[ADS_PGA_RANGES] = {
  GAIN_SIXTEEN, GAIN_EIGHT, GAIN_FOUR, GAIN_TWO, GAIN_ONE, GAIN_TWOTHIRDS};

// Zero-length write. out: true if the chip ACKed its address.
static bool ack(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

static bool isSingleEnded(ADS1115Mux m) {
  return m == ADS1115Mux::SINGLE_0 || m == ADS1115Mux::SINGLE_1 ||
         m == ADS1115Mux::SINGLE_2 || m == ADS1115Mux::SINGLE_3;
}

// Config register mux field for a channel's mux mode.
static uint16_t muxBits(ADS1115Mux m) {
  switch (m) {
    case ADS1115Mux::DIFF_0_3: return ADS1X15_REG_CONFIG_MUX_DIFF_0_3;
    case ADS1115Mux::DIFF_1_3: return ADS1X15_REG_CONFIG_MUX_DIFF_1_3;
    case ADS1115Mux::DIFF_2_3: return ADS1X15_REG_CONFIG_MUX_DIFF_2_3;
    case ADS1115Mux::SINGLE_0: return ADS1X15_REG_CONFIG_MUX_SINGLE_0;
    case ADS1115Mux::SINGLE_1: return ADS1X15_REG_CONFIG_MUX_SINGLE_1;
    case ADS1115Mux::SINGLE_2: return ADS1X15_REG_CONFIG_MUX_SINGLE_2;
    case ADS1115Mux::SINGLE_3: return ADS1X15_REG_CONFIG_MUX_SINGLE_3;
    case ADS1115Mux::DIFF_0_1:
    default:                   return ADS1X15_REG_CONFIG_MUX_DIFF_0_1;
  }
}

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

  Log::kvf(TAG, "ads.ready devices=%d channels=%d",
           (int)_devices.size(), (int)channelCount());

  SensorRegistry::add("current", "ADS1115 CT channels (RMS current)",
    [](ShellOutput out, void* ctx) {
      static_cast<ADS1115Module*>(ctx)->sensorRead(out);
    }, this, true);
}

// ---------------------------------------------------------------------------

// Bring up one ADS1115 at the given address and load its channel list. A chip
// that fails to init is kept (channels visible in status) and probed each read.
void ADS1115Module::addDevice(uint8_t address, JsonArray channels) {
  ADS1115Device dev;
  dev.address = address;
  dev.ads     = new Adafruit_ADS1115();
  dev.ok      = dev.ads->begin(address, &Wire);
  if (!dev.ok) {
    Log::kvfe(TAG, "ads.device_missing addr=0x%02X", address);
  }
  loadChannels(channels, dev.channels, address);
  _devices.push_back(dev);
}

// ---------------------------------------------------------------------------

// Parse channel definitions (mux pair, gain, clamp ratio, coupling) into dst.
void ADS1115Module::loadChannels(JsonArray src, std::vector<ADS1115Channel>& dst, uint8_t address) {
  if (src.isNull()) return;

  for (JsonObject ch : src) {
    const char* cname = ch["name"] | "";
    const char* muxS  = ch["mux"]  | "A0_A1";
    float       gain  = ch["gain"] | 1.024f;
    float       clamp = ch["clamp_a_per_v"] | 30.0f;  // SCT-013-030 default
    if (clamp <= 0.0f) {                              // bad config -> default
      Log::kvfw(TAG, "ads.clamp_invalid name=%s reason=nonpositive", cname);
      clamp = 30.0f;
    }

    ADS1115Channel c;
    strncpy(c.name, cname, sizeof(c.name) - 1);
    c.name[sizeof(c.name) - 1] = '\0';
    c.mux        = muxFromString(muxS);
    c.gain       = adsPgaFullScaleV(gain);
    c.gainEnum   = GAIN_BY_PGA[adsPgaIndex(gain)];
    c.clampAPerV = clamp;
    c.dc         = ch["dc"] | false;
    dst.push_back(c);

    if (c.gain != gain) {
      Log::kvfw(TAG, "ads.gain_rounded name=%s requested=%.3f fs=%.3f", cname, gain, c.gain);
    }
    Log::kvf(TAG, "ads.channel addr=0x%02X name=%s mux=%s gain=%.3f clamp=%g coupling=%s",
             address, cname, muxS, c.gain, clamp, c.dc ? "dc" : "ac");
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
    bool up = true;
    for (auto& ch : dev.channels) {
      float   rmsV = 0.0f;
      int16_t raw  = 0;
      if (up) up = readChannelRms(dev, ch, rmsV, raw);
      if (!up) {
        snprintf(line, sizeof(line), "  %-12s: device 0x%02X offline", ch.name, dev.address);
        out(line);
        continue;
      }
      float current = rmsV * ch.clampAPerV;
      float power   = current * _lineVoltage;
      snprintf(line, sizeof(line),
               "  %-12s: %.4f V %s  %.2f A  %.1f W",
               ch.name, rmsV, ch.dc ? "mean" : "rms", current, power);
      out(line);
    }
  }
}

// ---------------------------------------------------------------------------

// Read every channel on every answering device and publish current/power.
void ADS1115Module::readAndPublish() {
  if (channelCount() == 0) return;

  JsonDocument doc;
  JsonArray    arr = doc["channels"].to<JsonArray>();

  for (auto& dev : _devices) {
    bool up = true;
    for (auto& ch : dev.channels) {
      float   rmsV = 0.0f;
      int16_t raw  = 0;
      if (up) up = readChannelRms(dev, ch, rmsV, raw);
      if (!up) continue;
      // Clamp ratio is amps per 1 V output (e.g. SCT-013-030 = 30, SCT-013-005 = 5).
      float current = rmsV * ch.clampAPerV;
      float power   = current * _lineVoltage;

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

    char perTopic[MQTT_TOPIC_CAP], val[16];
    if (MQTTClient::formatTopic(perTopic, sizeof(perTopic), "%s/sensor/current/%s", prefix, slug)) {
      snprintf(val, sizeof(val), "%.2f", (float)(ch["current_a"] | 0.0f));
      MQTTClient::publish(perTopic, val);
    }

    if (MQTTClient::formatTopic(perTopic, sizeof(perTopic), "%s/sensor/power/%s", prefix, slug)) {
      snprintf(val, sizeof(val), "%.1f", (float)(ch["power_w"] | 0.0f));
      MQTTClient::publish(perTopic, val);
    }
  }

  // Combined JSON topic (for Lua EventBus, SD logging, backwards compat).
  // Dynamic buffer - 8 channels across two devices outgrow any fixed size and
  // a truncated payload would publish malformed JSON.
  char topic[MQTT_TOPIC_CAP];
  if (MQTTClient::formatTopic(topic, sizeof(topic), "%s/sensor/current", prefix)) {
    String payload;
    payload.reserve(measureJson(doc) + 1);
    serializeJson(doc, payload);
    MQTTClient::publish(topic, payload.c_str());
  }

  EventBus::publish("current", doc.as<JsonObject>());
}

// ---------------------------------------------------------------------------

// Probe, sample, probe again, and re-probe a timeout to tell a NACK from a stuck
// conversion. out: true with the reading in V and last raw count, false = offline.
bool ADS1115Module::readChannelRms(ADS1115Device& dev, ADS1115Channel& ch,
                                   float& rmsV, int16_t& raw) {
  const char* lost = nullptr;
  if (!ack(dev.address))                                       lost = "nack";
  else if (!sampleRms(*dev.ads, ch, RMS_SAMPLES, rmsV, raw))   lost = ack(dev.address) ? "timeout" : "nack";
  else if (!ack(dev.address))                                  lost = "nack";
  markOnline(dev, lost == nullptr, lost);
  return lost == nullptr;
}

// N single-shot conversions at 860 SPS (30 cover ~2 cycles at 60 Hz), each
// waited on bounded: a NACKed read never sets the bit readADC_* spins on.
bool ADS1115Module::sampleRms(Adafruit_ADS1115& ads, ADS1115Channel& ch,
                              int samples, float& rmsV, int16_t& raw) {
  ads.setGain(ch.gainEnum);
  ads.setDataRate(RATE_ADS1115_860SPS);

  AdsRms acc;
  for (int i = 0; i < samples; i++) {
    ads.startADCReading(muxBits(ch.mux), /*continuous=*/false);
    bool done = adsWaitReady([&ads]() { return ads.conversionComplete(); },
                             []() { return (uint32_t)micros(); },
                             ADS_CONVERSION_TIMEOUT_US);
    if (!done) return false;
    raw = ads.getLastConversionResults();
    adsRmsAdd(acc, raw);
  }
  if (ch.dc) {
    rmsV = adsMeanVolts(acc, ch.gain);
    return true;
  }
  rmsV = adsRmsAcVolts(acc, ch.gain);
  const AdsClip clip = isSingleEnded(ch.mux) ? adsClipAtGround(acc) : AdsClip::Unknown;
  if (clip != AdsClip::Unknown) markClipped(ch, clip == AdsClip::Clipped);
  return true;
}

// Record a read result in dev.ok, logging only a change of state.
void ADS1115Module::markOnline(ADS1115Device& dev, bool up, const char* reason) {
  if (dev.ok == up) return;
  dev.ok = up;
  if (up) {
    Log::kvf(TAG, "ads.device.state_change addr=0x%02X from=offline to=online", dev.address);
  } else {
    Log::kvfw(TAG, "ads.device.state_change addr=0x%02X from=online to=offline reason=%s",
              dev.address, reason);
  }
}

// Record a swing's ground clipping in ch.clipped, logging only a change.
void ADS1115Module::markClipped(ADS1115Channel& ch, bool clipped) {
  if (ch.clipped == clipped) return;
  ch.clipped = clipped;
  if (clipped) Log::kvfw(TAG, "ads.clipped name=%s reason=single_ended_no_bias", ch.name);
  else         Log::kvf(TAG, "ads.clip_cleared name=%s", ch.name);
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
  Log::kvfw(TAG, "ads.mux_unknown value=%s", s);
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
