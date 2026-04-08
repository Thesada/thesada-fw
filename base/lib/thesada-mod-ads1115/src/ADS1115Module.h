// thesada-fw - ADS1115Module.h
// ADS1115 16-bit ADC module. Reads configurable differential (or single-ended)
// channels, publishes readings to MQTT and the EventBus on a configurable
// interval. Channel list is defined at runtime in config.json - add or remove
// entries without recompiling.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <thesada_config.h>

#ifdef ENABLE_ADS1115

#include <Module.h>
#include <Adafruit_ADS1X15.h>
#include <vector>

// Mux mode resolved from the config.json "mux" string.
enum class ADS1115Mux : uint8_t {
  DIFF_0_1, DIFF_0_3, DIFF_1_3, DIFF_2_3,   // differential pairs
  SINGLE_0, SINGLE_1, SINGLE_2, SINGLE_3     // single-ended vs GND
};

// One configured input channel.
struct ADS1115Channel {
  char        name[32];    // human name (e.g. "house_pump")
  ADS1115Mux  mux;         // resolved mux mode
  float       gain;        // PGA full-scale in V (e.g. 1.024)
  adsGain_t   gainEnum;    // resolved Adafruit gain constant
};

class ADS1115Module : public Module {
public:
  void        begin() override;
  void        loop()  override;
  const char* name()  override { return "ADS1115"; }
  void        status(ShellOutput out) override;

private:
  void        loadChannels();
  void        readAndPublish();
  int16_t     readChannel(const ADS1115Channel& ch);
  float       readRmsVoltage(ADS1115Channel& ch, int samples = 30);
  adsGain_t   gainFromFloat(float g);
  ADS1115Mux  muxFromString(const char* s);

  Adafruit_ADS1115            _ads;
  std::vector<ADS1115Channel> _channels;
  uint32_t                    _intervalMs   = 60000;
  uint32_t                    _lastRead     = 0;
  float                       _lineVoltage  = 120.0f;
};

#endif // ENABLE_ADS1115
