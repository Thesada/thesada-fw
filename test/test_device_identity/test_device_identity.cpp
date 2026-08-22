// Host-native unit tests for device_identity_policy.h (first-boot identity).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "device_identity_policy.h"

void setUp(void) {}
void tearDown(void) {}

static const uint8_t kMac[6]  = {0xDC, 0xB4, 0xD9, 0x1A, 0xCD, 0x28};
static const uint8_t kMac2[6] = {0x48, 0x27, 0xE2, 0xE8, 0xB4, 0x34};

// --- identityHexEncode ------------------------------------------------------

void test_hex_encodes_lowercase(void) {
  char out[13];
  TEST_ASSERT_TRUE(identityHexEncode(kMac, 6, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("dcb4d91acd28", out);
}

void test_hex_encodes_zero_and_ff(void) {
  const uint8_t b[2] = {0x00, 0xFF};
  char out[5];
  TEST_ASSERT_TRUE(identityHexEncode(b, 2, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("00ff", out);
}

// Truncation must fail, not short-write: a clipped key is a different identity.
void test_hex_rejects_short_buffer(void) {
  char out[12];
  TEST_ASSERT_FALSE(identityHexEncode(kMac, 6, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("", out);
  char exact[13];
  TEST_ASSERT_TRUE(identityHexEncode(kMac, 6, exact, sizeof(exact)));
}

void test_hex_degenerate_inputs(void) {
  char out[8];
  TEST_ASSERT_FALSE(identityHexEncode(nullptr, 6, out, sizeof(out)));
  TEST_ASSERT_FALSE(identityHexEncode(kMac, 6, nullptr, 8));
  TEST_ASSERT_FALSE(identityHexEncode(kMac, 6, out, 0));
  TEST_ASSERT_TRUE(identityHexEncode(kMac, 0, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("", out);
}

// --- identityDeviceIdFromMac ------------------------------------------------

void test_device_id_from_mac(void) {
  char id[IDENTITY_ID_CAP];
  TEST_ASSERT_TRUE(identityDeviceIdFromMac(kMac, id, sizeof(id)));
  TEST_ASSERT_EQUAL_STRING("thesada-dcb4d91acd28", id);
}

// Two boards must not collide - the whole point of using all six bytes.
void test_device_id_differs_per_mac(void) {
  char a[IDENTITY_ID_CAP], b[IDENTITY_ID_CAP];
  TEST_ASSERT_TRUE(identityDeviceIdFromMac(kMac, a, sizeof(a)));
  TEST_ASSERT_TRUE(identityDeviceIdFromMac(kMac2, b, sizeof(b)));
  TEST_ASSERT_EQUAL_STRING("thesada-4827e2e8b434", b);
  TEST_ASSERT_TRUE(strcmp(a, b) != 0);
}

// Same MAC, same id, every boot - regeneration would orphan the pairing.
void test_device_id_is_stable(void) {
  char a[IDENTITY_ID_CAP], b[IDENTITY_ID_CAP];
  identityDeviceIdFromMac(kMac, a, sizeof(a));
  identityDeviceIdFromMac(kMac, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING(a, b);
}

void test_device_id_rejects_short_buffer(void) {
  char small[IDENTITY_ID_CAP - 1];
  TEST_ASSERT_FALSE(identityDeviceIdFromMac(kMac, small, sizeof(small)));
  TEST_ASSERT_EQUAL_STRING("", small);
}

void test_device_id_degenerate_inputs(void) {
  char id[IDENTITY_ID_CAP];
  TEST_ASSERT_FALSE(identityDeviceIdFromMac(nullptr, id, sizeof(id)));
  TEST_ASSERT_FALSE(identityDeviceIdFromMac(kMac, nullptr, IDENTITY_ID_CAP));
  TEST_ASSERT_FALSE(identityDeviceIdFromMac(kMac, id, 0));
}

// --- identityDeviceIdValid --------------------------------------------------

void test_valid_accepts_generated_id(void) {
  char id[IDENTITY_ID_CAP];
  identityDeviceIdFromMac(kMac, id, sizeof(id));
  TEST_ASSERT_TRUE(identityDeviceIdValid(id));
  TEST_ASSERT_TRUE(identityDeviceIdValid("thesada-000000000000"));
  TEST_ASSERT_TRUE(identityDeviceIdValid("thesada-ffffffffffff"));
}

void test_valid_rejects_wrong_length(void) {
  TEST_ASSERT_FALSE(identityDeviceIdValid("thesada-dcb4d91acd2"));    // 11
  TEST_ASSERT_FALSE(identityDeviceIdValid("thesada-dcb4d91acd280"));  // 13
  TEST_ASSERT_FALSE(identityDeviceIdValid("thesada-"));
}

// Uppercase hex is not what this firmware writes, so it is not our shape.
void test_valid_rejects_non_lowercase_hex(void) {
  TEST_ASSERT_FALSE(identityDeviceIdValid("thesada-DCB4D91ACD28"));
  TEST_ASSERT_FALSE(identityDeviceIdValid("thesada-dcb4d91acdzz"));
  TEST_ASSERT_FALSE(identityDeviceIdValid("thesada-dcb4d9 1acd2"));
}

// A hand-edited config.json name must not pass as a generated identity.
void test_valid_rejects_legacy_names(void) {
  TEST_ASSERT_FALSE(identityDeviceIdValid("thesada-node"));
  TEST_ASSERT_FALSE(identityDeviceIdValid("thesada-owb-debug"));
  TEST_ASSERT_FALSE(identityDeviceIdValid("node"));
  TEST_ASSERT_FALSE(identityDeviceIdValid(""));
  TEST_ASSERT_FALSE(identityDeviceIdValid(nullptr));
}

// --- identityKeyMaterialPresent ---------------------------------------------

// An erased or half-written NVS read is all zeroes and must not look like a
// generated keypair, or the device would publish a null public key.
void test_key_material_rejects_all_zero(void) {
  uint8_t z[32] = {0};
  TEST_ASSERT_FALSE(identityKeyMaterialPresent(z, sizeof(z)));
}

void test_key_material_accepts_any_nonzero(void) {
  uint8_t k[32] = {0};
  k[31] = 0x01;
  TEST_ASSERT_TRUE(identityKeyMaterialPresent(k, sizeof(k)));
  k[31] = 0x00;
  k[0]  = 0x80;
  TEST_ASSERT_TRUE(identityKeyMaterialPresent(k, sizeof(k)));
}

void test_key_material_degenerate_inputs(void) {
  uint8_t k[4] = {1, 2, 3, 4};
  TEST_ASSERT_FALSE(identityKeyMaterialPresent(nullptr, 32));
  TEST_ASSERT_FALSE(identityKeyMaterialPresent(k, 0));
}

// --- identityNodeNameUnique -------------------------------------------------

// The operator label alone is enough: it is what nodeName() returns.
void test_node_name_unique_with_configured_name(void) {
  TEST_ASSERT_TRUE(identityNodeNameUnique("attic-node", ""));
  TEST_ASSERT_TRUE(identityNodeNameUnique("attic-node", "thesada-dcb4d91acd28"));
}

void test_node_name_unique_with_device_id_only(void) {
  TEST_ASSERT_TRUE(identityNodeNameUnique("", "thesada-dcb4d91acd28"));
}

// Neither set is the shared-literal case every broker path must refuse.
void test_node_name_not_unique_when_both_absent(void) {
  TEST_ASSERT_FALSE(identityNodeNameUnique("", ""));
  TEST_ASSERT_FALSE(identityNodeNameUnique(nullptr, nullptr));
  TEST_ASSERT_FALSE(identityNodeNameUnique(nullptr, ""));
  TEST_ASSERT_FALSE(identityNodeNameUnique("", nullptr));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_hex_encodes_lowercase);
  RUN_TEST(test_hex_encodes_zero_and_ff);
  RUN_TEST(test_hex_rejects_short_buffer);
  RUN_TEST(test_hex_degenerate_inputs);
  RUN_TEST(test_device_id_from_mac);
  RUN_TEST(test_device_id_differs_per_mac);
  RUN_TEST(test_device_id_is_stable);
  RUN_TEST(test_device_id_rejects_short_buffer);
  RUN_TEST(test_device_id_degenerate_inputs);
  RUN_TEST(test_valid_accepts_generated_id);
  RUN_TEST(test_valid_rejects_wrong_length);
  RUN_TEST(test_valid_rejects_non_lowercase_hex);
  RUN_TEST(test_valid_rejects_legacy_names);
  RUN_TEST(test_key_material_rejects_all_zero);
  RUN_TEST(test_key_material_accepts_any_nonzero);
  RUN_TEST(test_key_material_degenerate_inputs);
  RUN_TEST(test_node_name_unique_with_configured_name);
  RUN_TEST(test_node_name_unique_with_device_id_only);
  RUN_TEST(test_node_name_not_unique_when_both_absent);
  return UNITY_END();
}
