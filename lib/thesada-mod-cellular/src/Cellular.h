// thesada-fw - Cellular.h
// SIM7080G LTE-M/NB-IoT connectivity with modem-native MQTT over TLS.
// PMU (AXP2101), TinyGSM registration, AT+SM* MQTT stack.
// Network-selection policy (when to activate vs yield to WiFi) lives in
// CellularModule. This class owns the modem itself: bring-up, recovery
// from cellular drops, and a publish gate it can be told to open or close.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>

class Cellular {
public:
  // Initialise PMU, wake modem, check SIM, write CA cert, register, MQTT connect.
  // Idempotent: returns immediately if already started.
  // Side-effect: leaves the publish gate open on success.
  static void begin();

  // Cellular-only recovery loop: re-register on network drop, reconnect MQTT
  // if dropped while network is up. Does NOT touch WiFi; CellularModule owns
  // the WiFi-vs-cellular selection.
  static void loop();

  // True when modem-MQTT is connected AND the publish gate is open.
  // Used by every Cellular::publish() guard and by external subscribers.
  static bool connected();

  // Open or close the publish gate without disconnecting the modem.
  // Closed gate keeps the MQTT session warm so re-takeover after a brief
  // WiFi flap can resume publishing without a fresh registration cycle.
  static void setPublishGate(bool open);

  // Last cached signal quality from AT+CSQ. 0..31 valid, 99 = unknown.
  // Updated by Cellular::loop() when started; returns 99 before first sample.
  static int  getSignalQuality();

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
  static void sampleSignalQuality();

  static bool     _started;
  static bool     _mqttConnected;
  static bool     _publishGate;
  static bool     _hasCACert;
  static int      _signalQuality;
  static uint32_t _lastSignalSample;
};
