// thesada-fw - MeshtasticFrame.h
// Meshtastic interop shim: runtime channel state + AES-CTR (mbedtls) around
// the pure codec in thesada-core's meshtastic_frame.h. No RadioLib types
// here - just byte buffers - so it composes with LoRaRadio.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>

#ifdef ENABLE_LORA

#include <meshtastic_frame.h>

namespace mesh {

// Channel crypto state, derived once from config at module begin().
struct Channel {
  uint8_t key[32] = {0};
  size_t  keyLen  = 0;   // 16 = AES128, 32 = AES256, 0 = crypto off
  uint8_t hash    = 0;
};

enum class Parse { Ok, NotOurs, ForeignPort, Malformed };

// Build a broadcast TEXT_MESSAGE_APP frame on ch into out. Returns total
// length, 0 on overflow.
size_t buildText(const Channel& ch, const char* text, uint32_t fromNode,
                 uint32_t packetId, uint8_t* out, size_t cap);

// Decode a received frame against ch. Ok: text + fromNode + packetId set.
// ForeignPort: clean decode for another app - portnum set for the caller's log.
Parse parseText(const Channel& ch, const uint8_t* buf, size_t len,
                String& text, uint32_t& fromNode, uint32_t& portnum,
                uint32_t& packetId);

}  // namespace mesh

#endif  // ENABLE_LORA
