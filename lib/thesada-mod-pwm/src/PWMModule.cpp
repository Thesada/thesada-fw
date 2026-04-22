// thesada-fw - PWMModule.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include <thesada_config.h>
#include "PWMModule.h"
#include <Config.h>
#include <EventBus.h>
#include <MQTTClient.h>
#include <Log.h>
#include <ModuleRegistry.h>

static const char* TAG = "PWM";

uint8_t  PWMModule::_pin          = 16;
uint8_t  PWMModule::_channel      = 0;
uint32_t PWMModule::_frequency    = 25000;
uint8_t  PWMModule::_resolution   = 8;
float    PWMModule::_currentLevel = 0.0f;

// Configure LEDC PWM channel from config and subscribe to control events
void PWMModule::begin() {
  JsonObject cfg = Config::get();

  _pin        = cfg["pwm"]["pin"]          | 16;
  _channel    = cfg["pwm"]["channel"]      | 0;
  _frequency  = cfg["pwm"]["frequency_hz"] | 25000;
  _resolution = cfg["pwm"]["resolution"]   | 8;

  // arduino-esp32 3.x unified LEDC API to pin-based (ledcAttach), 2.x was channel-based.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(_pin, _frequency, _resolution);
  ledcWrite(_pin, 0);
#else
  ledcSetup(_channel, _frequency, _resolution);
  ledcAttachPin(_pin, _channel);
  ledcWrite(_channel, 0);
#endif

  char msg[64];
  snprintf(msg, sizeof(msg), "Ready - GPIO%d, %luHz, %d-bit", _pin, _frequency, _resolution);
  Log::info(TAG, msg);

  subscribeEventBus();
  subscribeMQTT();
}

// No periodic work - PWM is event-driven
void PWMModule::loop() {}

// Set PWM output to a 0.0-1.0 level and publish the new state via MQTT
void PWMModule::setLevel(float level) {
  level = constrain(level, 0.0f, 1.0f);
  _currentLevel = level;

  uint32_t maxVal = (1 << _resolution) - 1;
  uint32_t duty   = (uint32_t)(level * maxVal);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(_pin, duty);
#else
  ledcWrite(_channel, duty);
#endif

  char msg[64];
  snprintf(msg, sizeof(msg), "Level set to %.0f%% (duty %lu/%lu)", level * 100, duty, maxVal);
  Log::info(TAG, msg);

  JsonObject  cfg    = Config::get();
  const char* prefix = cfg["mqtt"]["topic_prefix"] | "thesada/node";
  char topic[64];
  snprintf(topic, sizeof(topic), "%s/sensor/pwm", prefix);

  char payload[64];
  snprintf(payload, sizeof(payload), "{\"level\":%.2f,\"percent\":%.0f}", level, level * 100);
  MQTTClient::publish(topic, payload);
}

// Listen for pwm_set events on the EventBus
void PWMModule::subscribeEventBus() {
  EventBus::subscribe("pwm_set", [](JsonObject data) {
    float level = data["level"] | -1.0f;
    if (level < 0.0f || level > 1.0f) {
      Log::warn(TAG, "Invalid level in pwm_set event - expected 0.0-1.0");
      return;
    }
    PWMModule::setLevel(level);
  });
}

// Register MQTT command topic for PWM control
void PWMModule::subscribeMQTT() {
  Log::info(TAG, "MQTT cmd topic ready (requires MQTTClient subscription dispatch)");
}

// Handle an incoming MQTT PWM command (expects 0-100 percent string)
void PWMModule::onMQTTCommand(const char* payload) {
  int percent = atoi(payload);
  if (percent < 0 || percent > 100) {
    Log::warn(TAG, "Invalid PWM percent - expected 0-100");
    return;
  }
  setLevel(percent / 100.0f);
}

// Report PWM module status
void PWMModule::status(ShellOutput out) {
  JsonObject cfg = Config::get();
  int pin = cfg["pwm"]["pin"] | -1;
  int freq = cfg["pwm"]["frequency_hz"] | 0;
  char line[64];
  snprintf(line, sizeof(line), "pin=%d  freq=%dHz", pin, freq);
  out(line);
}

#ifdef ENABLE_PWM
MODULE_REGISTER(PWMModule, PRIORITY_OUTPUT)
#endif
