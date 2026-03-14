// thesada-fw — MQTTClient.h
// MQTT over TLS port 8883
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

class MQTTClient {
public:
  static void begin();
  static void loop();
  static void publish(const char* topic, const char* payload);
  static bool connected();
};
