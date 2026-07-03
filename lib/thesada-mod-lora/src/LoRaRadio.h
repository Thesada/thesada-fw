// thesada-fw - LoRaRadio.h
// Thin SX1262 wrapper over RadioLib. Header exposes NO RadioLib types so it can
// be included alongside thesada-core's Module.h without the global `class Module`
// name colliding with RadioLib's own `Module` HAL class. All RadioLib usage is
// confined to LoRaRadio.cpp.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>

class SX1262;    // RadioLib, forward-declared
class SPIClass;  // Arduino SPI, forward-declared

// Result of a receive poll. data is a raw byte buffer, NOT a C string:
// Meshtastic frames are encrypted binary and legally contain NUL bytes
// (String-based readData truncates at the first one).
struct LoRaRx {
  bool    received = false;  // a packet was read without a read error
  bool    crcErr   = false;  // RX_DONE but CRC failed
  uint8_t data[256];         // SX1262 max LoRa packet is 255 B
  size_t  len = 0;
  float   rssi = 0.0f;
  float   snr  = 0.0f;
};

class LoRaRadio {
public:
  // Set up SPI, the SX1262, and the MCU-driven RF switch. Returns the RadioLib
  // status code (0 = RADIOLIB_ERR_NONE). DIO1 is left NC (poll-mode).
  int  begin(uint8_t nss, uint8_t sck, uint8_t miso, uint8_t mosi,
             uint8_t nrst, uint8_t busy, uint8_t rxen, uint8_t txen,
             float freq, float bw, uint8_t sf, uint8_t cr,
             uint8_t syncWord, int8_t power, uint16_t preamble, float tcxo);

  // Transmit a packet, polling TX_DONE up to timeoutMs. Returns true on TX_DONE.
  bool  transmit(const char* msg, uint32_t timeoutMs);
  // Binary variant (Meshtastic frames contain NUL bytes, so strlen won't do).
  bool  transmit(const uint8_t* data, size_t len, uint32_t timeoutMs);
  void  startReceive();
  void  standby();

  // Poll for a received packet. Returns true if RX_DONE fired (fills out + re-arms RX).
  bool  poll(LoRaRx& out);

  // Instantaneous RSSI (band noise floor / activity), dBm.
  float instantRssi();

private:
  SPIClass* _spi   = nullptr;
  SX1262*   _radio = nullptr;
};
