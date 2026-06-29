// thesada-fw - MeshtasticFrame.cpp
// Meshtastic LongFast frame codec. Constants reverse-engineered from Meshtastic
// firmware v2.7.x and bench-verified against stock 2.7.26.
// SPDX-License-Identifier: GPL-3.0-only
#include "MeshtasticFrame.h"

#ifdef ENABLE_LORA

#include "mbedtls/aes.h"

namespace {

// Meshtastic default-channel key (AES128); the 1-byte PSK index 1 expands to this.
const uint8_t KEY[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                         0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};

// AES128-CTR in place. nonce = packetId(8 LE) || fromNode(4 LE) || 0(4), counter in low 4 bytes.
void aesCtr(uint32_t fromNode, uint32_t packetId, uint8_t* buf, size_t n) {
  uint8_t nonce[16];
  memset(nonce, 0, 16);
  memcpy(nonce, &packetId, 4);
  memcpy(nonce + 8, &fromNode, 4);
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, KEY, 128);
  size_t off = 0;
  uint8_t stream[16] = {0};
  mbedtls_aes_crypt_ctr(&ctx, n, &off, nonce, stream, buf, buf);
  mbedtls_aes_free(&ctx);
}

uint32_t readVarint(const uint8_t* p, size_t pl, size_t& i) {
  uint32_t v = 0;
  int shift = 0;
  while (i < pl && shift < 32) {
    uint8_t b = p[i++];
    v |= (uint32_t)(b & 0x7f) << shift;
    if (!(b & 0x80)) break;
    shift += 7;
  }
  return v;
}

}  // namespace

size_t mesh::buildText(const char* text, uint32_t fromNode, uint32_t packetId, uint8_t* out, size_t cap) {
  size_t tlen = strlen(text);
  uint8_t data[240];
  size_t dn = 0;
  if (tlen > sizeof(data) - 6) return 0;
  data[dn++] = 0x08;                 // Data.portnum (field 1, varint)
  data[dn++] = 0x01;                 // = TEXT_MESSAGE_APP
  data[dn++] = 0x12;                 // Data.payload (field 2, len-delimited)
  data[dn++] = (uint8_t)tlen;
  memcpy(data + dn, text, tlen);
  dn += tlen;
  aesCtr(fromNode, packetId, data, dn);

  if (cap < 16 + dn) return 0;
  size_t pn = 0;
  uint32_t to = 0xFFFFFFFF;          // broadcast
  memcpy(out + pn, &to, 4); pn += 4;
  memcpy(out + pn, &fromNode, 4); pn += 4;
  memcpy(out + pn, &packetId, 4); pn += 4;
  out[pn++] = 0x63;                  // flags: hopLimit 3 | hopStart 3<<5
  out[pn++] = mesh::CHANNEL;
  out[pn++] = 0x00;                  // next_hop
  out[pn++] = 0x00;                  // relay_node
  memcpy(out + pn, data, dn); pn += dn;
  return pn;
}

bool mesh::parseText(const uint8_t* buf, size_t len, String& text, uint32_t& fromNode) {
  if (len < 18) return false;
  if (buf[13] != mesh::CHANNEL) return false;
  uint32_t from, id;
  memcpy(&from, buf + 4, 4);
  memcpy(&id, buf + 8, 4);

  uint8_t pt[240];
  size_t pl = len - 16;
  if (pl > sizeof(pt)) return false;
  memcpy(pt, buf + 16, pl);
  aesCtr(from, id, pt, pl);          // CTR decrypt is the same op as encrypt

  uint8_t portnum = 0;
  const uint8_t* payload = nullptr;
  size_t payLen = 0;
  size_t i = 0;
  while (i < pl) {
    uint8_t tag = pt[i++];
    uint8_t field = tag >> 3, wire = tag & 0x07;
    if (field == 1 && wire == 0) {
      portnum = (uint8_t)readVarint(pt, pl, i);
    } else if (field == 2 && wire == 2) {
      uint32_t l = readVarint(pt, pl, i);
      if (i + l > pl) return false;
      payload = pt + i;
      payLen = l;
      i += l;
    } else {
      return false;
    }
  }
  if (portnum != 1 || !payload) return false;
  text = "";
  for (size_t k = 0; k < payLen; k++) text += (char)payload[k];
  fromNode = from;
  return true;
}

#endif  // ENABLE_LORA
