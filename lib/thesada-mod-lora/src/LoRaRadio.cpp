// thesada-fw - LoRaRadio.cpp
// RadioLib SX1262 implementation. This TU deliberately does NOT include any
// thesada-core header, so RadioLib's `class Module` is the only `Module` in
// scope (thesada-core defines its own). Sequence matches the validated bench
// probe: FSPI, DIO1 NC (poll getIrqFlags), MCU RF switch via setRfSwitchPins.
// SPDX-License-Identifier: GPL-3.0-only
#include "LoRaRadio.h"
#include <RadioLib.h>
#include <SPI.h>

// Bring up SPI + radio core, then the RF switch. Returns RadioLib status.
int LoRaRadio::begin(uint8_t nss, uint8_t sck, uint8_t miso, uint8_t mosi,
                     uint8_t nrst, uint8_t busy, uint8_t rxen, uint8_t txen,
                     float freq, float bw, uint8_t sf, uint8_t cr,
                     uint8_t syncWord, int8_t power, uint16_t preamble, float tcxo) {
  _spi = new SPIClass(FSPI);
  _spi->begin(sck, miso, mosi, nss);
  _radio = new SX1262(new Module(nss, RADIOLIB_NC, nrst, busy, *_spi));

  int st = _radio->begin(freq, bw, sf, cr, syncWord, power, preamble, tcxo);
  if (st == RADIOLIB_ERR_NONE) _radio->setRfSwitchPins(rxen, txen);
  return st;
}

// Poll TX_DONE up to timeoutMs, then finish. Leaves the radio in standby.
static bool waitTxDone(SX1262* radio, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  bool done = false;
  while (millis() - t0 < timeoutMs) {
    if (radio->getIrqFlags() & RADIOLIB_SX126X_IRQ_TX_DONE) { done = true; break; }
    delay(2);
  }
  radio->finishTransmit();
  return done;
}

bool LoRaRadio::transmit(const char* msg, uint32_t timeoutMs) {
  _radio->standby();
  if (_radio->startTransmit(msg) != RADIOLIB_ERR_NONE) {
    _radio->finishTransmit();
    return false;
  }
  return waitTxDone(_radio, timeoutMs);
}

bool LoRaRadio::transmit(const uint8_t* data, size_t len, uint32_t timeoutMs) {
  _radio->standby();
  if (_radio->startTransmit(const_cast<uint8_t*>(data), len) != RADIOLIB_ERR_NONE) {
    _radio->finishTransmit();
    return false;
  }
  return waitTxDone(_radio, timeoutMs);
}

void LoRaRadio::startReceive() { _radio->startReceive(); }
void LoRaRadio::standby()      { _radio->standby(); }

// DIO1 is NC on this board, so RX completion is detected by polling the IRQ
// flags rather than an interrupt. Re-arms continuous RX before returning.
bool LoRaRadio::poll(LoRaRx& out) {
  uint32_t f = _radio->getIrqFlags();
  if (!(f & RADIOLIB_SX126X_IRQ_RX_DONE)) return false;

  out.crcErr   = (f & RADIOLIB_SX126X_IRQ_CRC_ERR) != 0;
  out.len      = _radio->getPacketLength();
  if (out.len > sizeof(out.data)) out.len = sizeof(out.data);
  out.received = (_radio->readData(out.data, out.len) == RADIOLIB_ERR_NONE);
  out.rssi     = _radio->getRSSI();
  out.snr      = _radio->getSNR();
  _radio->startReceive();
  return true;
}

float LoRaRadio::instantRssi() { return _radio->getRSSI(false); }
