// thesada-fw - PWMModule.h
// LEDC PWM output - controllable via MQTT and EventBus
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <Module.h>

class PWMModule : public Module {
public:
  void begin() override;
  void loop() override;
  const char* name() override { return "PWMModule"; }
  void status(ShellOutput out) override;

  static void setLevel(float level); // 0.0 - 1.0

private:
  static void onMQTTCommand(const char* payload);
  void        subscribeEventBus();
  void        subscribeMQTT();

  static uint8_t  _pin;
  static uint8_t  _channel;   // LEDC channel (0–15)
  static uint32_t _frequency;
  static uint8_t  _resolution;
  static float    _currentLevel;
};
