// thesada-fw - MeshtasticFrame.cpp
// Thin mbedtls AES-CTR + Arduino String shim over the pure Meshtastic codec
// (thesada-core meshtastic_frame.h). Byte layout is unit-tested on the host;
// the crypto is CTR-symmetric and bench-verified against stock 2.7.26.
// SPDX-License-Identifier: GPL-3.0-only
#include "MeshtasticFrame.h"

#ifdef ENABLE_LORA

#include "mbedtls/aes.h"

namespace {

// AES-CTR in place with the channel key. keyLen 0 = plaintext channel (no-op).
void aesCtr(const mesh::Channel& ch, uint32_t fromNode, uint32_t packetId,
            uint8_t* buf, size_t n) {
  if (ch.keyLen == 0) return;
  uint8_t nonce[mesh::NONCE_LEN];
  mesh::nonceBuild(packetId, fromNode, nonce);
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, ch.key, (unsigned)ch.keyLen * 8);
  size_t off = 0;
  uint8_t stream[16] = {0};
  mbedtls_aes_crypt_ctr(&ctx, n, &off, nonce, stream, buf, buf);
  mbedtls_aes_free(&ctx);
}

}  // namespace

size_t mesh::buildText(const Channel& ch, const char* text, uint32_t fromNode,
                       uint32_t packetId, uint8_t* out, size_t cap) {
  uint8_t data[240];  // 255 LoRa max - 16 header, rounded up
  size_t dn = dataEncode(PORT_TEXT, (const uint8_t*)text, strlen(text),
                         data, sizeof(data));
  if (!dn) return 0;
  aesCtr(ch, fromNode, packetId, data, dn);

  Header h;
  h.dest = BROADCAST;
  h.src = fromNode;
  h.packetId = packetId;
  h.channelHash = ch.hash;
  return frameBuild(h, data, dn, out, cap);
}

mesh::Parse mesh::parseText(const Channel& ch, const uint8_t* buf, size_t len,
                            String& text, uint32_t& fromNode, uint32_t& portnum,
                            uint32_t& packetId) {
  portnum = 0;
  packetId = 0;
  Header h;
  const uint8_t* body = nullptr;
  size_t blen = 0;
  if (!frameSplit(buf, len, h, body, blen) || blen == 0) return Parse::Malformed;
  if (h.channelHash != ch.hash) return Parse::NotOurs;

  uint8_t pt[240];
  if (blen > sizeof(pt)) return Parse::Malformed;
  memcpy(pt, body, blen);
  aesCtr(ch, h.src, h.packetId, pt, blen);  // CTR decrypt = encrypt

  const uint8_t* payload = nullptr;
  size_t plen = 0;
  if (!dataDecode(pt, blen, portnum, payload, plen)) return Parse::Malformed;
  if (portnum != PORT_TEXT) return Parse::ForeignPort;

  text = "";
  text.reserve(plen);
  for (size_t k = 0; k < plen; k++) text += (char)payload[k];
  fromNode = h.src;
  packetId = h.packetId;
  return Parse::Ok;
}

#endif  // ENABLE_LORA
