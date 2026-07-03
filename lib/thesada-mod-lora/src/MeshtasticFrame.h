// thesada-fw - MeshtasticFrame.h
// Meshtastic interop: LongFast/US default-channel PHY constants + frame
// build/parse (16B PacketHeader + AES128-CTR over a protobuf Data message).
// No RadioLib types here - just byte buffers - so it composes with LoRaRadio.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>

#ifdef ENABLE_LORA

namespace mesh {

// LongFast preset on the US default channel (slot 19 -> 906.875 MHz).
constexpr float    FREQ_MHZ = 906.875f;
constexpr float    BW_KHZ   = 250.0f;
constexpr uint8_t  SF       = 11;
constexpr uint8_t  CR       = 5;
constexpr uint8_t  SYNC     = 0x2b;
constexpr uint16_t PREAMBLE = 16;
constexpr uint8_t  CHANNEL  = 0x08;  // xorHash("LongFast") ^ xorHash(default key)

// Build a broadcast TEXT_MESSAGE_APP frame into out. Returns total length, 0 on overflow.
size_t buildText(const char* text, uint32_t fromNode, uint32_t packetId, uint8_t* out, size_t cap);

// If buf is a text message on our channel, set text + fromNode and return true.
bool parseText(const uint8_t* buf, size_t len, String& text, uint32_t& fromNode);

}  // namespace mesh

#endif  // ENABLE_LORA
