// Host-native unit tests for meshtastic_frame.h (pure Meshtastic wire codec).
// Golden values (channelHash 0x08, header layout, nonce layout) are the
// bench-verified ones from the stock 2.7.26 interop session.
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <string.h>
#include "meshtastic_frame.h"

void setUp(void) {}
void tearDown(void) {}

void test_varint_roundtrip(void) {
  const uint32_t vals[] = {0, 1, 127, 128, 300, 16383, 16384, 0xFFFFFFFFu};
  for (uint32_t v : vals) {
    uint8_t buf[8];
    size_t n = mesh::varintWrite(v, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n >= 1 && n <= 5);
    size_t i = 0;
    bool ok = false;
    TEST_ASSERT_EQUAL_UINT32(v, mesh::varintRead(buf, n, i, ok));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT(n, i);
  }
}

void test_varint_bounds(void) {
  uint8_t buf[8];
  TEST_ASSERT_EQUAL_UINT(0, mesh::varintWrite(300, buf, 1));  // needs 2 bytes
  buf[0] = 0x80;                                              // truncated continuation
  size_t i = 0;
  bool ok = true;
  mesh::varintRead(buf, 1, i, ok);
  TEST_ASSERT_FALSE(ok);
  memset(buf, 0x80, 6);                                       // overlong (>5 bytes)
  i = 0;
  mesh::varintRead(buf, 6, i, ok);
  TEST_ASSERT_FALSE(ok);
}

void test_header_roundtrip_and_flags_golden(void) {
  mesh::Header h;
  h.dest = mesh::BROADCAST;
  h.src = 0x1ACD28AA;
  h.packetId = 0xDEADBEEF;
  h.hopLimit = 3;
  h.hopStart = 3;
  h.channelHash = 0x08;
  uint8_t buf[mesh::HEADER_LEN];
  mesh::headerPack(h, buf);
  TEST_ASSERT_EQUAL_HEX8(0x63, buf[12]);   // bench golden: hop 3 | start 3<<5
  TEST_ASSERT_EQUAL_HEX8(0xFF, buf[0]);    // broadcast LE
  TEST_ASSERT_EQUAL_HEX8(0xAA, buf[4]);    // src LE low byte first
  TEST_ASSERT_EQUAL_HEX8(0xEF, buf[8]);    // packetId LE

  mesh::Header r;
  TEST_ASSERT_TRUE(mesh::headerParse(buf, sizeof(buf), r));
  TEST_ASSERT_EQUAL_UINT32(h.dest, r.dest);
  TEST_ASSERT_EQUAL_UINT32(h.src, r.src);
  TEST_ASSERT_EQUAL_UINT32(h.packetId, r.packetId);
  TEST_ASSERT_EQUAL_UINT8(3, r.hopLimit);
  TEST_ASSERT_EQUAL_UINT8(3, r.hopStart);
  TEST_ASSERT_FALSE(r.wantAck);
  TEST_ASSERT_FALSE(r.viaMQTT);
  TEST_ASSERT_EQUAL_HEX8(0x08, r.channelHash);

  TEST_ASSERT_FALSE(mesh::headerParse(buf, 15, r));  // short frame
}

void test_header_flag_bits(void) {
  mesh::Header h;
  h.wantAck = true;
  h.viaMQTT = true;
  h.hopLimit = 7;
  h.hopStart = 5;
  uint8_t buf[mesh::HEADER_LEN];
  mesh::headerPack(h, buf);
  mesh::Header r;
  TEST_ASSERT_TRUE(mesh::headerParse(buf, sizeof(buf), r));
  TEST_ASSERT_TRUE(r.wantAck);
  TEST_ASSERT_TRUE(r.viaMQTT);
  TEST_ASSERT_EQUAL_UINT8(7, r.hopLimit);
  TEST_ASSERT_EQUAL_UINT8(5, r.hopStart);
}

void test_data_roundtrip_short(void) {
  const char* text = "hello mesh";
  uint8_t buf[64];
  size_t n = mesh::dataEncode(mesh::PORT_TEXT, (const uint8_t*)text, strlen(text),
                              buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  uint32_t port = 0;
  const uint8_t* payload = nullptr;
  size_t plen = 0;
  TEST_ASSERT_TRUE(mesh::dataDecode(buf, n, port, payload, plen));
  TEST_ASSERT_EQUAL_UINT32(mesh::PORT_TEXT, port);
  TEST_ASSERT_EQUAL_UINT(strlen(text), plen);
  TEST_ASSERT_EQUAL_MEMORY(text, payload, plen);
}

// Regression for the v0 bug: payload > 127 B needs a 2-byte length varint;
// v0 wrote the raw length byte and emitted invalid protobuf.
void test_data_roundtrip_long_payload(void) {
  uint8_t payload[200];
  for (size_t k = 0; k < sizeof(payload); k++) payload[k] = (uint8_t)('a' + k % 26);
  uint8_t buf[240];
  size_t n = mesh::dataEncode(mesh::PORT_TEXT, payload, sizeof(payload), buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_UINT(1 + 1 + 1 + 2 + sizeof(payload), n);  // tag, port, tag, 2B len, payload
  TEST_ASSERT_EQUAL_HEX8(0xC8, buf[3]);                // len 200 varint low byte
  TEST_ASSERT_EQUAL_HEX8(0x01, buf[4]);                // len 200 varint high byte
  uint32_t port = 0;
  const uint8_t* p = nullptr;
  size_t plen = 0;
  TEST_ASSERT_TRUE(mesh::dataDecode(buf, n, port, p, plen));
  TEST_ASSERT_EQUAL_UINT(sizeof(payload), plen);
  TEST_ASSERT_EQUAL_MEMORY(payload, p, plen);
}

void test_data_decode_rejects_malformed(void) {
  uint32_t port;
  const uint8_t* p;
  size_t plen;
  const uint8_t lenPastEnd[] = {0x08, 0x01, 0x12, 0x20, 0x61};  // len 32, 1 byte left
  TEST_ASSERT_FALSE(mesh::dataDecode(lenPastEnd, 5, port, p, plen));
  const uint8_t noPayload[] = {0x08, 0x01};                 // portnum only
  TEST_ASSERT_FALSE(mesh::dataDecode(noPayload, 2, port, p, plen));
  const uint8_t unkPastEnd[] = {0x08, 0x01, 0x12, 0x01, 0x61, 0x35, 0xAA};  // fixed32 field, 1 byte left
  TEST_ASSERT_FALSE(mesh::dataDecode(unkPastEnd, 7, port, p, plen));
  const uint8_t group[] = {0x08, 0x01, 0x12, 0x01, 0x61, 0x0b};  // wire 3 (group)
  TEST_ASSERT_FALSE(mesh::dataDecode(group, 6, port, p, plen));
}

// Length varint 0xFFFFFFFF: on the 32-bit target the old additive bounds
// check (i + l > len) wrapped and accepted a ~4 GB plen from a 240-byte
// frame, and the text-copy loop then walked off the stack (LoadProhibited).
// The remaining-bytes form (l > len - i) cannot wrap at any size_t width;
// this host test pins the contract even though 64-bit size_t masks the
// original wrap.
void test_data_decode_rejects_overflowing_length(void) {
  uint32_t port;
  const uint8_t* p;
  size_t plen;
  // field 2 payload with 5-byte varint len = 0xFFFFFFFF, one trailing byte
  const uint8_t hugePayload[] = {0x08, 0x01, 0x12, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x61};
  TEST_ASSERT_FALSE(mesh::dataDecode(hugePayload, sizeof(hugePayload), port, p, plen));
  // same varint on the unknown-field skip path (field 15, wire 2)
  const uint8_t hugeUnknown[] = {0x08, 0x01, 0x7A, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x61};
  TEST_ASSERT_FALSE(mesh::dataDecode(hugeUnknown, sizeof(hugeUnknown), port, p, plen));
}

// Meshtastic 2.5+ appends Data.bitfield (field 9, varint) to every text
// message - decode must skip unknown fields, not reject them. This exact
// shape (portnum, payload, bitfield=1) is what a 2.7 peer sends.
void test_data_decode_skips_unknown_fields(void) {
  uint32_t port;
  const uint8_t* p;
  size_t plen;
  const uint8_t bitfield[] = {0x08, 0x01, 0x12, 0x03, 'a', 'b', 'c', 0x48, 0x01};
  TEST_ASSERT_TRUE(mesh::dataDecode(bitfield, sizeof(bitfield), port, p, plen));
  TEST_ASSERT_EQUAL_UINT32(1, port);
  TEST_ASSERT_EQUAL_UINT(3, plen);
  TEST_ASSERT_EQUAL_MEMORY("abc", p, 3);
  // Unknown fields of every skippable wire type, before the known ones.
  const uint8_t mixed[] = {0x48, 0x01,                            // field 9 varint
                           0x1a, 0x02, 0xde, 0xad,                // field 3 len-delim
                           0x25, 1, 2, 3, 4,                      // field 4 fixed32
                           0x31, 1, 2, 3, 4, 5, 6, 7, 8,          // field 6 fixed64
                           0x08, 0x01, 0x12, 0x01, 'x'};
  TEST_ASSERT_TRUE(mesh::dataDecode(mixed, sizeof(mixed), port, p, plen));
  TEST_ASSERT_EQUAL_UINT32(1, port);
  TEST_ASSERT_EQUAL_UINT(1, plen);
  TEST_ASSERT_EQUAL_HEX8('x', p[0]);
}

void test_data_decode_foreign_portnum(void) {
  uint8_t buf[32];
  size_t n = mesh::dataEncode(300, (const uint8_t*)"x", 1, buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  uint32_t port = 0;
  const uint8_t* p = nullptr;
  size_t plen = 0;
  TEST_ASSERT_TRUE(mesh::dataDecode(buf, n, port, p, plen));  // clean parse...
  TEST_ASSERT_EQUAL_UINT32(300, port);                        // ...caller filters portnum
}

void test_channel_hash_golden(void) {
  // Bench golden: (LongFast, default key) -> 0x08.
  TEST_ASSERT_EQUAL_HEX8(0x08, mesh::channelHash("LongFast", mesh::DEFAULT_KEY,
                                                 sizeof(mesh::DEFAULT_KEY)));
  // Crypto-off channel hashes name only.
  TEST_ASSERT_EQUAL_HEX8(mesh::xorFold((const uint8_t*)"LongFast", 8),
                         mesh::channelHash("LongFast", nullptr, 0));
}

void test_nonce_layout_golden(void) {
  uint8_t nonce[mesh::NONCE_LEN];
  mesh::nonceBuild(0x11223344, 0xAABBCCDD, nonce);
  const uint8_t want[16] = {0x44, 0x33, 0x22, 0x11, 0, 0, 0, 0,
                            0xDD, 0xCC, 0xBB, 0xAA, 0, 0, 0, 0};
  TEST_ASSERT_EQUAL_MEMORY(want, nonce, 16);
}

void test_frame_build_split_roundtrip(void) {
  mesh::Header h;
  h.src = 42;
  h.packetId = 7;
  h.channelHash = 0x08;
  const uint8_t body[] = {1, 2, 3, 0, 5};   // NUL inside stays intact
  uint8_t frame[64];
  size_t n = mesh::frameBuild(h, body, sizeof(body), frame, sizeof(frame));
  TEST_ASSERT_EQUAL_UINT(mesh::HEADER_LEN + sizeof(body), n);

  mesh::Header r;
  const uint8_t* rbody = nullptr;
  size_t rlen = 0;
  TEST_ASSERT_TRUE(mesh::frameSplit(frame, n, r, rbody, rlen));
  TEST_ASSERT_EQUAL_UINT32(42, r.src);
  TEST_ASSERT_EQUAL_UINT(sizeof(body), rlen);
  TEST_ASSERT_EQUAL_MEMORY(body, rbody, rlen);

  uint8_t tiny[8];
  TEST_ASSERT_EQUAL_UINT(0, mesh::frameBuild(h, body, sizeof(body), tiny, sizeof(tiny)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_varint_roundtrip);
  RUN_TEST(test_varint_bounds);
  RUN_TEST(test_header_roundtrip_and_flags_golden);
  RUN_TEST(test_header_flag_bits);
  RUN_TEST(test_data_roundtrip_short);
  RUN_TEST(test_data_roundtrip_long_payload);
  RUN_TEST(test_data_decode_rejects_malformed);
  RUN_TEST(test_data_decode_rejects_overflowing_length);
  RUN_TEST(test_data_decode_skips_unknown_fields);
  RUN_TEST(test_data_decode_foreign_portnum);
  RUN_TEST(test_channel_hash_golden);
  RUN_TEST(test_nonce_layout_golden);
  RUN_TEST(test_frame_build_split_roundtrip);
  return UNITY_END();
}
