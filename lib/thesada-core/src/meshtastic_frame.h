// thesada-fw - meshtastic_frame.h
// Pure Meshtastic wire-format codec: 16-byte PacketHeader pack/parse, protobuf
// varint + Data message encode/decode, channel xor-hash, AES-CTR nonce layout.
// No Arduino/IDF/mbedtls deps, so it is host-unit-testable; MeshtasticFrame
// (thesada-mod-lora) wraps the crypto and String glue around it. Wire layout
// derived from Meshtastic firmware 2.7.x sources and bench-verified against
// a stock 2.7.26 peer.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace mesh {

constexpr size_t   HEADER_LEN   = 16;
constexpr uint32_t BROADCAST    = 0xFFFFFFFFu;
constexpr uint32_t PORT_TEXT    = 1;   // portnum TEXT_MESSAGE_APP
constexpr size_t   NONCE_LEN    = 16;

// Meshtastic default-channel AES128 key; 1-byte PSK index 1 expands to this.
constexpr uint8_t DEFAULT_KEY[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                                     0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};

inline void putLE32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
inline uint32_t getLE32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 16-byte on-air PacketHeader. flags byte: bits 0-2 hopLimit, bit 3 wantAck,
// bit 4 viaMQTT, bits 5-7 hopStart.
struct Header {
  uint32_t dest      = BROADCAST;
  uint32_t src       = 0;
  uint32_t packetId  = 0;
  uint8_t  hopLimit  = 3;
  bool     wantAck   = false;
  bool     viaMQTT   = false;
  uint8_t  hopStart  = 3;
  uint8_t  channelHash = 0;
  uint8_t  nextHop   = 0;
  uint8_t  relayNode = 0;
};

inline void headerPack(const Header& h, uint8_t out[HEADER_LEN]) {
  putLE32(out,     h.dest);
  putLE32(out + 4, h.src);
  putLE32(out + 8, h.packetId);
  out[12] = (uint8_t)((h.hopLimit & 0x07) | (h.wantAck ? 0x08 : 0) |
                      (h.viaMQTT ? 0x10 : 0) | ((h.hopStart & 0x07) << 5));
  out[13] = h.channelHash;
  out[14] = h.nextHop;
  out[15] = h.relayNode;
}

inline bool headerParse(const uint8_t* buf, size_t len, Header& h) {
  if (!buf || len < HEADER_LEN) return false;
  h.dest     = getLE32(buf);
  h.src      = getLE32(buf + 4);
  h.packetId = getLE32(buf + 8);
  h.hopLimit = buf[12] & 0x07;
  h.wantAck  = (buf[12] & 0x08) != 0;
  h.viaMQTT  = (buf[12] & 0x10) != 0;
  h.hopStart = (buf[12] >> 5) & 0x07;
  h.channelHash = buf[13];
  h.nextHop  = buf[14];
  h.relayNode = buf[15];
  return true;
}

// Protobuf base-128 varint (32-bit range). Returns bytes written, 0 if cap
// is too small.
inline size_t varintWrite(uint32_t v, uint8_t* out, size_t cap) {
  size_t n = 0;
  do {
    if (n >= cap) return 0;
    uint8_t b = v & 0x7f;
    v >>= 7;
    out[n++] = v ? (b | 0x80) : b;
  } while (v);
  return n;
}

// Reads a varint at i and advances i. ok=false on truncation or a varint
// longer than 5 bytes (32-bit overlong).
inline uint32_t varintRead(const uint8_t* p, size_t len, size_t& i, bool& ok) {
  uint32_t v = 0;
  unsigned shift = 0;
  ok = false;
  while (i < len && shift <= 28) {
    uint8_t b = p[i++];
    v |= (uint32_t)(b & 0x7f) << shift;
    if (!(b & 0x80)) { ok = true; return v; }
    shift += 7;
  }
  return 0;
}

// Encode a protobuf Data message: field 1 portnum (varint), field 2 payload
// (length-delimited). Plaintext - the caller encrypts. Returns total length,
// 0 on overflow.
inline size_t dataEncode(uint32_t portnum, const uint8_t* payload, size_t plen,
                         uint8_t* out, size_t cap) {
  size_t n = 0;
  if (n >= cap) return 0;
  out[n++] = 0x08;                                   // field 1, wire type varint
  size_t w = varintWrite(portnum, out + n, cap - n);
  if (!w) return 0;
  n += w;
  if (n >= cap) return 0;
  out[n++] = 0x12;                                   // field 2, wire type len-delimited
  w = varintWrite((uint32_t)plen, out + n, cap - n); // real varint: >127 B payloads need 2 bytes
  if (!w) return 0;
  n += w;
  if (n + plen > cap) return 0;
  memcpy(out + n, payload, plen);
  return n + plen;
}

// Decode a Data message (already decrypted). Unknown fields are skipped by
// wire type per protobuf rules - Data keeps growing (2.5 added bitfield #9
// to every text message), and rejecting unknowns drops real mesh traffic.
// False only on actual malformation (truncation, overlong varint, groups).
inline bool dataDecode(const uint8_t* buf, size_t len, uint32_t& portnum,
                       const uint8_t*& payload, size_t& plen) {
  portnum = 0;
  payload = nullptr;
  plen = 0;
  size_t i = 0;
  bool ok = false;
  while (i < len) {
    uint8_t tag = buf[i++];
    uint8_t field = tag >> 3, wire = tag & 0x07;
    if (field == 1 && wire == 0) {
      portnum = varintRead(buf, len, i, ok);
      if (!ok) return false;
    } else if (field == 2 && wire == 2) {
      uint32_t l = varintRead(buf, len, i, ok);
      if (!ok || i + l > len) return false;
      payload = buf + i;
      plen = l;
      i += l;
    } else {
      switch (wire) {
        case 0:                                        // varint
          varintRead(buf, len, i, ok);
          if (!ok) return false;
          break;
        case 1:                                        // fixed64
          if (i + 8 > len) return false;
          i += 8;
          break;
        case 2: {                                      // length-delimited
          uint32_t l = varintRead(buf, len, i, ok);
          if (!ok || i + l > len) return false;
          i += l;
          break;
        }
        case 5:                                        // fixed32
          if (i + 4 > len) return false;
          i += 4;
          break;
        default:                                       // groups / invalid
          return false;
      }
    }
  }
  return payload != nullptr;
}

// Meshtastic channel hash: xor-fold of the channel name bytes xor the same
// over the PSK bytes. (LongFast, default key) -> 0x08.
inline uint8_t xorFold(const uint8_t* p, size_t n) {
  uint8_t h = 0;
  for (size_t k = 0; k < n; k++) h ^= p[k];
  return h;
}
inline uint8_t channelHash(const char* name, const uint8_t* psk, size_t pskLen) {
  return xorFold((const uint8_t*)name, name ? strlen(name) : 0) ^ xorFold(psk, pskLen);
}

// AES-CTR nonce: packetId as uint64 LE (upper 4 bytes zero), fromNode LE,
// extraNonce zero (non-PKC channel crypto).
inline void nonceBuild(uint32_t packetId, uint32_t fromNode, uint8_t out[NONCE_LEN]) {
  memset(out, 0, NONCE_LEN);
  putLE32(out, packetId);
  putLE32(out + 8, fromNode);
}

// header + body -> contiguous frame. Returns total length, 0 on overflow.
inline size_t frameBuild(const Header& h, const uint8_t* body, size_t blen,
                         uint8_t* out, size_t cap) {
  if (HEADER_LEN + blen > cap) return 0;
  headerPack(h, out);
  memcpy(out + HEADER_LEN, body, blen);
  return HEADER_LEN + blen;
}

// Split a received frame into header + body view. Body may be empty.
inline bool frameSplit(const uint8_t* buf, size_t len, Header& h,
                       const uint8_t*& body, size_t& blen) {
  if (!headerParse(buf, len, h)) return false;
  body = buf + HEADER_LEN;
  blen = len - HEADER_LEN;
  return true;
}

}  // namespace mesh
