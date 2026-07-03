// Host-native unit tests for meshtastic_radio.h (preset/region tables, slot
// math, base64, PSK expansion). Golden: LongFast/US -> slot 19 -> 906.875 MHz,
// the bench-verified interop frequency.
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <string.h>
#include "meshtastic_radio.h"

void setUp(void) {}
void tearDown(void) {}

void test_preset_lookup(void) {
  const mesh::Preset* p = mesh::presetFind("LongFast");
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_FLOAT(250.0f, p->bwKhz);
  TEST_ASSERT_EQUAL_UINT8(11, p->sf);
  TEST_ASSERT_EQUAL_UINT8(5, p->cr);
  TEST_ASSERT_NOT_NULL(mesh::presetFind("ShortTurbo"));
  TEST_ASSERT_NOT_NULL(mesh::presetFind("LongMod"));
  TEST_ASSERT_NULL(mesh::presetFind("LongModerate"));  // Meshtastic slot-name is LongMod
  TEST_ASSERT_NULL(mesh::presetFind("bogus"));
  TEST_ASSERT_NULL(mesh::presetFind(nullptr));
}

void test_region_lookup(void) {
  const mesh::Region* r = mesh::regionFind("US");
  TEST_ASSERT_NOT_NULL(r);
  TEST_ASSERT_EQUAL_FLOAT(902.0f, r->freqStart);
  TEST_ASSERT_EQUAL_FLOAT(928.0f, r->freqEnd);
  TEST_ASSERT_NOT_NULL(mesh::regionFind("EU_868"));
  TEST_ASSERT_NOT_NULL(mesh::regionFind("ANZ"));
  TEST_ASSERT_NULL(mesh::regionFind("XX"));
}

void test_longfast_us_golden(void) {
  const mesh::Region* us = mesh::regionFind("US");
  TEST_ASSERT_EQUAL_UINT16(104, mesh::numChannels(*us, 250.0f));
  uint16_t slot = 0;
  float f = mesh::slotFreqMhz(*us, 250.0f, "LongFast", slot);
  TEST_ASSERT_EQUAL_UINT16(19, slot);              // UI shows slot 20
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 906.875f, f);   // bench-verified interop freq
}

void test_eu868_single_slot(void) {
  const mesh::Region* eu = mesh::regionFind("EU_868");
  TEST_ASSERT_EQUAL_UINT16(1, mesh::numChannels(*eu, 250.0f));
  uint16_t slot = 9;
  float f = mesh::slotFreqMhz(*eu, 250.0f, "LongFast", slot);
  TEST_ASSERT_EQUAL_UINT16(0, slot);               // only one slot fits
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 869.525f, f);
}

void test_preset_too_wide_fails_closed(void) {
  const mesh::Region* eu = mesh::regionFind("EU_868");
  TEST_ASSERT_EQUAL_UINT16(0, mesh::numChannels(*eu, 500.0f));  // ShortTurbo/LongTurbo
  uint16_t slot = 9;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, mesh::slotFreqMhz(*eu, 500.0f, "ShortTurbo", slot));
}

void test_channel_name_changes_slot(void) {
  const mesh::Region* us = mesh::regionFind("US");
  uint16_t a = 0, b = 0;
  mesh::slotFreqMhz(*us, 250.0f, "LongFast", a);
  mesh::slotFreqMhz(*us, 250.0f, "MyPrivateNet", b);
  TEST_ASSERT_TRUE(a != b);   // djb2 spreads distinct names to distinct slots
}

void test_b64_decode(void) {
  uint8_t out[40];
  TEST_ASSERT_EQUAL_UINT(1, mesh::b64Decode("AQ==", out, sizeof(out)));
  TEST_ASSERT_EQUAL_HEX8(0x01, out[0]);
  // The well-known Meshtastic default PSK, base64.
  size_t n = mesh::b64Decode("1PG7OiApB1nwvP+rz05pAQ==", out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT(16, n);
  TEST_ASSERT_EQUAL_MEMORY(mesh::DEFAULT_KEY, out, 16);

  TEST_ASSERT_EQUAL_UINT((size_t)-1, mesh::b64Decode("not base64!", out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT((size_t)-1, mesh::b64Decode("AQ==AQ", out, sizeof(out)));  // data after pad
  uint8_t tiny[4];
  TEST_ASSERT_EQUAL_UINT((size_t)-1,
                         mesh::b64Decode("1PG7OiApB1nwvP+rz05pAQ==", tiny, sizeof(tiny)));
}

void test_psk_expand_default_and_index(void) {
  uint8_t key[32];
  size_t klen = 99;
  TEST_ASSERT_TRUE(mesh::pskExpand(nullptr, 0, key, klen));  // empty -> default key
  TEST_ASSERT_EQUAL_UINT(16, klen);
  TEST_ASSERT_EQUAL_MEMORY(mesh::DEFAULT_KEY, key, 16);

  uint8_t idx0 = 0;                                          // explicit crypto off
  TEST_ASSERT_TRUE(mesh::pskExpand(&idx0, 1, key, klen));
  TEST_ASSERT_EQUAL_UINT(0, klen);

  uint8_t idx1 = 1;                                          // index 1 = default key
  TEST_ASSERT_TRUE(mesh::pskExpand(&idx1, 1, key, klen));
  TEST_ASSERT_EQUAL_UINT(16, klen);
  TEST_ASSERT_EQUAL_MEMORY(mesh::DEFAULT_KEY, key, 16);

  uint8_t idx2 = 2;                                          // index 2 bumps last byte
  TEST_ASSERT_TRUE(mesh::pskExpand(&idx2, 1, key, klen));
  TEST_ASSERT_EQUAL_UINT(16, klen);
  TEST_ASSERT_EQUAL_HEX8(mesh::DEFAULT_KEY[15] + 1, key[15]);
  TEST_ASSERT_EQUAL_MEMORY(mesh::DEFAULT_KEY, key, 15);
}

void test_psk_expand_raw_keys(void) {
  uint8_t key[32];
  size_t klen = 0;
  uint8_t k16[16], k32[32];
  memset(k16, 0x5a, sizeof(k16));
  memset(k32, 0xa5, sizeof(k32));
  TEST_ASSERT_TRUE(mesh::pskExpand(k16, 16, key, klen));     // AES128 as-is
  TEST_ASSERT_EQUAL_UINT(16, klen);
  TEST_ASSERT_EQUAL_MEMORY(k16, key, 16);
  TEST_ASSERT_TRUE(mesh::pskExpand(k32, 32, key, klen));     // AES256 as-is
  TEST_ASSERT_EQUAL_UINT(32, klen);
  TEST_ASSERT_EQUAL_MEMORY(k32, key, 32);
  TEST_ASSERT_FALSE(mesh::pskExpand(k16, 5, key, klen));     // odd length rejected
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_preset_lookup);
  RUN_TEST(test_region_lookup);
  RUN_TEST(test_longfast_us_golden);
  RUN_TEST(test_eu868_single_slot);
  RUN_TEST(test_preset_too_wide_fails_closed);
  RUN_TEST(test_channel_name_changes_slot);
  RUN_TEST(test_b64_decode);
  RUN_TEST(test_psk_expand_default_and_index);
  RUN_TEST(test_psk_expand_raw_keys);
  return UNITY_END();
}
