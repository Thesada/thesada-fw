// thesada-fw - LoRaModule.h
// SX1262 LoRa radio module (Ebyte E22-900MM22S, base-lora-carrier U6). Poll-mode
// (DIO1 is NC on this board), MCU-driven RXEN/TXEN RF switch, internal TCXO.
// Radio access is via LoRaRadio so RadioLib's `Module` stays out of this TU.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <thesada_config.h>

#ifdef ENABLE_LORA

// Explicit path, not <Module.h>: RadioLib (pulled by LoRaRadio.cpp in this lib)
// ships its own Module.h, and its include dir would otherwise shadow the
// base-class header here. LoRaRadio.h exposes no RadioLib types, so this TU
// never sees RadioLib's Module.
#include "../../thesada-core/src/Module.h"
#include "LoRaRadio.h"

class LoRaModule : public Module {
public:
  void        begin() override;
  void        loop()  override;
  const char* name()  override { return "LoRa"; }
  const char* configKey() override { return "lora"; }
  void        status(ShellOutput out) override;

private:
  bool transmit(const char* msg);
  void publishRx(const LoRaRx& rx);
  void setListening(bool on);

  LoRaRadio _radio;
  bool      _ok        = false;
  bool      _listening = false;
  bool      _meshtastic = false;  // frame as Meshtastic LongFast instead of raw text
  uint32_t  _nodeNum    = 0;      // our Meshtastic node id (low 4 bytes of MAC by default)

  float    _freq     = 915.0f;
  float    _bw       = 125.0f;
  uint8_t  _sf       = 9;
  uint8_t  _cr       = 7;
  int8_t   _power    = 14;
  uint16_t _preamble = 8;
  uint8_t  _syncWord = 0x12;   // RadioLib SX126X private sync word

  float    _lastRssi = 0.0f;
  float    _lastSnr  = 0.0f;
  uint32_t _rxCount  = 0;
};

#endif // ENABLE_LORA
