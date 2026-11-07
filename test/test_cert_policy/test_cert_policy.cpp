// Host-native unit tests for cert_policy.h - PEM structural pre-flight, CN
// extraction and the cert.apply verdict. Does not cover the mbedtls pair
// check, which stays on-target.
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "cert_policy.h"

void setUp(void) {}
void tearDown(void) {}

static const char* CERT1 =
    "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n";
static const char* TRUNCATED =
    "-----BEGIN CERTIFICATE-----\nMIIB\n";
static const char* REVERSED =
    "-----END CERTIFICATE-----\nMIIB\n-----BEGIN CERTIFICATE-----\n";
static const char* KEY_PKCS8 =
    "-----BEGIN PRIVATE KEY-----\nMIIE\n-----END PRIVATE KEY-----\n";
static const char* KEY_RSA =
    "-----BEGIN RSA PRIVATE KEY-----\nMIIE\n-----END RSA PRIVATE KEY-----\n";
static const char* KEY_EC =
    "-----BEGIN EC PRIVATE KEY-----\nMHc\n-----END EC PRIVATE KEY-----\n";
static const char* KEY_ENC =
    "-----BEGIN ENCRYPTED PRIVATE KEY-----\nMIIE\n-----END ENCRYPTED PRIVATE KEY-----\n";
static const char* PUBKEY =
    "-----BEGIN PUBLIC KEY-----\nMFkw\n-----END PUBLIC KEY-----\n";

// ---- PEM structure -------------------------------------------------------

void test_well_formed_cert_recognised(void) {
  TEST_ASSERT_TRUE(pemLooksLikeCert(CERT1));
}

// Truncated mid-block must be rejected before mbedtls is handed it.
void test_truncated_cert_rejected(void) {
  TEST_ASSERT_FALSE(pemLooksLikeCert(TRUNCATED));
}

// END before BEGIN is not a block.
void test_reversed_markers_rejected(void) {
  TEST_ASSERT_FALSE(pemLooksLikeCert(REVERSED));
}

void test_empty_and_null_rejected(void) {
  TEST_ASSERT_FALSE(pemLooksLikeCert(""));
  TEST_ASSERT_FALSE(pemLooksLikeCert(nullptr));
  TEST_ASSERT_FALSE(pemLooksLikeKey(nullptr));
}

void test_all_four_key_headers_accepted(void) {
  TEST_ASSERT_TRUE(pemLooksLikeKey(KEY_PKCS8));
  TEST_ASSERT_TRUE(pemLooksLikeKey(KEY_RSA));
  TEST_ASSERT_TRUE(pemLooksLikeKey(KEY_EC));
  TEST_ASSERT_TRUE(pemLooksLikeKey(KEY_ENC));  // mbedtls accepts these too
}

// A public key is not a private key - this is the swap that must not pass.
void test_public_key_is_not_a_private_key(void) {
  TEST_ASSERT_FALSE(pemLooksLikeKey(PUBKEY));
}

// ---- Combined pre-flight -------------------------------------------------

void test_matching_shapes_pass_preflight(void) {
  TEST_ASSERT_TRUE(certKeyInputsUsable(CERT1, KEY_PKCS8));
}

// Swapped arguments: a key in the cert slot and vice versa.
void test_swapped_cert_and_key_rejected(void) {
  TEST_ASSERT_FALSE(certKeyInputsUsable(KEY_PKCS8, CERT1));
}

void test_missing_or_empty_inputs_rejected(void) {
  TEST_ASSERT_FALSE(certKeyInputsUsable(nullptr, KEY_PKCS8));
  TEST_ASSERT_FALSE(certKeyInputsUsable(CERT1, nullptr));
  TEST_ASSERT_FALSE(certKeyInputsUsable("", KEY_PKCS8));
  TEST_ASSERT_FALSE(certKeyInputsUsable(CERT1, ""));
}

void test_garbage_rejected(void) {
  TEST_ASSERT_FALSE(certKeyInputsUsable("not a pem", "also not a pem"));
}

// ---- CN extraction -------------------------------------------------------

void test_cn_extracted_from_dn(void) {
  char cn[64];
  TEST_ASSERT_TRUE(certExtractCn("C=CA, O=Thesada, CN=owb-debug", cn, sizeof(cn)));
  TEST_ASSERT_EQUAL_STRING("owb-debug", cn);
}

void test_cn_stops_at_next_field(void) {
  char cn[64];
  TEST_ASSERT_TRUE(certExtractCn("CN=owb-debug, O=Thesada", cn, sizeof(cn)));
  TEST_ASSERT_EQUAL_STRING("owb-debug", cn);
}

// An EMPTY CN is still a found marker: the caller's "(no CN)" fallback must not
// fire. This is what the inline code did, and what the first extraction broke.
void test_empty_cn_is_found_not_missing(void) {
  char cn[64];
  TEST_ASSERT_TRUE(certExtractCn("CN=", cn, sizeof(cn)));
  TEST_ASSERT_EQUAL_STRING("", cn);
  TEST_ASSERT_TRUE(certExtractCn("CN=,O=Thesada", cn, sizeof(cn)));
  TEST_ASSERT_EQUAL_STRING("", cn);
}

void test_no_cn_returns_false_and_empties(void) {
  char cn[64];
  memset(cn, 'x', sizeof(cn));
  TEST_ASSERT_FALSE(certExtractCn("C=CA, O=Thesada", cn, sizeof(cn)));
  TEST_ASSERT_EQUAL_STRING("", cn);
}

// Truncation must terminate, never run off the buffer.
void test_cn_truncates_safely(void) {
  char cn[5];
  TEST_ASSERT_TRUE(certExtractCn("CN=abcdefghij", cn, sizeof(cn)));
  TEST_ASSERT_EQUAL_STRING("abcd", cn);
}

void test_null_dn_and_zero_len_handled(void) {
  char cn[8];
  TEST_ASSERT_FALSE(certExtractCn(nullptr, cn, sizeof(cn)));
  TEST_ASSERT_FALSE(certExtractCn("CN=x", cn, 0));
}

// Documents the strstr weakness: an earlier field whose VALUE contains "CN="
// wins over the real CN. Preserved from the original, pinned so a later fix is
// a deliberate change rather than an accident.
void test_cn_substring_in_earlier_field_wins(void) {
  char cn[64];
  TEST_ASSERT_TRUE(certExtractCn("O=ACN=decoy, CN=real", cn, sizeof(cn)));
  TEST_ASSERT_EQUAL_STRING("decoy", cn);
}

// Same class: a CN containing a comma is cut at it.
void test_cn_with_comma_truncates(void) {
  char cn[64];
  TEST_ASSERT_TRUE(certExtractCn("CN=node\\,one, O=Thesada", cn, sizeof(cn)));
  TEST_ASSERT_EQUAL_STRING("node\\", cn);
}

// ---- cert.apply ----------------------------------------------------------

// cert.set stores halves unchecked: a mismatched pair must not be applied (#58).
void test_cert_apply_refuses_a_mismatched_pair(void) {
  TEST_ASSERT_TRUE(certApplyVerdict(true, PairCheck::Invalid) == CertApply::Mismatch);
}

void test_cert_apply_reboots_onto_a_valid_pair(void) {
  TEST_ASSERT_TRUE(certApplyVerdict(true, PairCheck::Valid) == CertApply::Reboot);
}

// A check the heap could not run is neither a pass nor a mismatch.
void test_cert_apply_low_heap_is_not_a_mismatch(void) {
  TEST_ASSERT_TRUE(certApplyVerdict(true, PairCheck::NoHeap) == CertApply::LowHeap);
}

// A missing half wins over any pair verdict.
void test_cert_apply_needs_both_halves(void) {
  TEST_ASSERT_TRUE(certApplyVerdict(false, PairCheck::Invalid) == CertApply::Missing);
  TEST_ASSERT_TRUE(certApplyVerdict(false, PairCheck::Valid) == CertApply::Missing);
  TEST_ASSERT_TRUE(certApplyVerdict(false, PairCheck::NoHeap) == CertApply::Missing);
}

// A refused cert.apply must open cert.clear on the same password session.
void test_invalid_pair_marks_the_stored_cert_broken(void) {
  TEST_ASSERT_TRUE(storedCertBrokenAfter(PairCheck::Invalid, false));
  TEST_ASSERT_TRUE(storedCertBrokenAfter(PairCheck::Invalid, true));
}

void test_valid_pair_clears_the_broken_verdict(void) {
  TEST_ASSERT_FALSE(storedCertBrokenAfter(PairCheck::Valid, true));
  TEST_ASSERT_FALSE(storedCertBrokenAfter(PairCheck::Valid, false));
}

// A full heap must not open cert.clear while a good cert sits in NVS.
void test_low_heap_keeps_the_last_verdict(void) {
  TEST_ASSERT_FALSE(storedCertBrokenAfter(PairCheck::NoHeap, false));
  TEST_ASSERT_TRUE(storedCertBrokenAfter(PairCheck::NoHeap, true));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_well_formed_cert_recognised);
  RUN_TEST(test_truncated_cert_rejected);
  RUN_TEST(test_reversed_markers_rejected);
  RUN_TEST(test_empty_and_null_rejected);
  RUN_TEST(test_all_four_key_headers_accepted);
  RUN_TEST(test_public_key_is_not_a_private_key);
  RUN_TEST(test_matching_shapes_pass_preflight);
  RUN_TEST(test_swapped_cert_and_key_rejected);
  RUN_TEST(test_missing_or_empty_inputs_rejected);
  RUN_TEST(test_garbage_rejected);
  RUN_TEST(test_cn_extracted_from_dn);
  RUN_TEST(test_cn_stops_at_next_field);
  RUN_TEST(test_empty_cn_is_found_not_missing);
  RUN_TEST(test_no_cn_returns_false_and_empties);
  RUN_TEST(test_cn_truncates_safely);
  RUN_TEST(test_null_dn_and_zero_len_handled);
  RUN_TEST(test_cn_substring_in_earlier_field_wins);
  RUN_TEST(test_cn_with_comma_truncates);
  RUN_TEST(test_cert_apply_refuses_a_mismatched_pair);
  RUN_TEST(test_cert_apply_reboots_onto_a_valid_pair);
  RUN_TEST(test_cert_apply_low_heap_is_not_a_mismatch);
  RUN_TEST(test_cert_apply_needs_both_halves);
  RUN_TEST(test_invalid_pair_marks_the_stored_cert_broken);
  RUN_TEST(test_valid_pair_clears_the_broken_verdict);
  RUN_TEST(test_low_heap_keeps_the_last_verdict);
  return UNITY_END();
}
