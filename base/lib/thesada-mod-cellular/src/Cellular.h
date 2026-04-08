// thesada-fw - Cellular.h
// SIM7080G LTE-M/NB-IoT connectivity with modem-native MQTT over TLS.
// PMU (AXP2101), TinyGSM registration, AT+SM* MQTT stack.
// Called by CellularModule when WiFi has fully failed.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>

class Cellular {
public:
  // Initialise PMU, wake modem, check SIM, write CA cert, register, MQTT connect.
  static void begin();

  // Recovery loop: re-register/MQTT on drop; periodic WiFi re-check.
  static void loop();

  // True when modem-MQTT is connected and ready to publish.
  static bool connected();

  // Publish via modem-native AT+SMPUB. Returns false if not connected.
  static bool publish(const char* topic, const char* payload);

private:
  static void initPMU();
  static void wakeModem();
  static bool writeCACert();
  static bool networkConnect();
  static bool mqttConnect();
  static bool isRegistered();
  static bool mqttIsConnected();

  static bool     _started;
  static bool     _mqttOk;
  static bool     _hasCACert;
  static uint32_t _lastWiFiCheck;
};
