// thesada-fw - ADS1115Module.h
// ADS1115 16-bit ADC module. Reads configurable differential (or single-ended)
// channels across one or more ADS1115 devices, publishing readings to MQTT and
// the EventBus on a configurable interval. Device and channel lists are defined
// at runtime in config.json - add or remove entries without recompiling.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <thesada_config.h>

#ifdef ENABLE_ADS1115

#include <Module.h>
#include <Adafruit_ADS1X15.h>
#include <ArduinoJson.h>
#include <vector>

// Mux mode resolved from the config.json "mux" string.
enum class ADS1115Mux : uint8_t {
  DIFF_0_1, DIFF_0_3, DIFF_1_3, DIFF_2_3,   // differential pairs
  SINGLE_0, SINGLE_1, SINGLE_2, SINGLE_3     // single-ended vs GND
};

// One configured input channel.
struct ADS1115Channel {
  char        name[32];    // human label, e.g. "pump_a" or "ct1"
  ADS1115Mux  mux;         // resolved mux mode
  float       gain;        // PGA full-scale in V (e.g. 1.024)
  adsGain_t   gainEnum;    // resolved Adafruit gain constant
  float       clampAPerV;  // CT clamp ratio, amps per 1 V output (default 30)
};

// One physical ADS1115 chip at a fixed I2C address with its own channel list.
struct ADS1115Device {
  uint8_t                     address;
  Adafruit_ADS1115*           ads = nullptr;  // heap, lives until reboot
  std::vector<ADS1115Channel> channels;
  bool                        ok  = false;     // begin() succeeded
};

class ADS1115Module : public Module {
public:
  void        begin() override;
  void        loop()  override;
  const char* name()  override { return "ADS1115"; }
  const char* configKey() override { return "ads1115"; }
  void        status(ShellOutput out) override;

  // One-shot read for the SensorRegistry callback. Prints one line per
  // configured channel with the RMS current reading.
  void        sensorRead(ShellOutput out);

private:
  void        addDevice(uint8_t address, JsonArray channels);
  void        loadChannels(JsonArray src, std::vector<ADS1115Channel>& dst, uint8_t address);
  void        readAndPublish();
  int16_t     readChannel(Adafruit_ADS1115& ads, const ADS1115Channel& ch);
  float       readRmsVoltage(Adafruit_ADS1115& ads, ADS1115Channel& ch, int samples = 30);
  adsGain_t   gainFromFloat(float g);
  ADS1115Mux  muxFromString(const char* s);
  size_t      channelCount() const;

  std::vector<ADS1115Device> _devices;
  uint32_t                   _intervalMs   = 60000;
  uint32_t                   _lastRead     = 0;
  float                      _lineVoltage  = 120.0f;
};

#endif // ENABLE_ADS1115
