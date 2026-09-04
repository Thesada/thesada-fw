// Host-native unit tests for ota_verify_policy.h - the OTA verification path
// decisions: TLS insecure guard, manifest validation, version predicate, digest
// compare.
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "ota_verify_policy.h"

void setUp(void) {}
void tearDown(void) {}

static const char* SHA_SHORT =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b85";  // 63, truncated
static const char* SHA_OK =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
static const char* SHA_OK_UPPER =
    "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855";
static const char* SHA_OTHER =
    "0000000000000000000000000000000000000000000000000000000000000000";

// ---- TLS insecure guard --------------------------------------------------

// A CA cert wins regardless of the opt-in - never silently downgrade.
void test_ca_present_is_verified(void) {
  TEST_ASSERT_EQUAL(OTA_TLS_VERIFIED, otaTlsMode(true, false));
  TEST_ASSERT_EQUAL(OTA_TLS_VERIFIED, otaTlsMode(true, true));
}

// The whole point of the guard: no cert and no opt-in means OTA stays down.
void test_no_ca_without_optin_refuses(void) {
  TEST_ASSERT_EQUAL(OTA_TLS_REFUSED, otaTlsMode(false, false));
}

// Insecure is a distinct state from refused, not a synonym.
void test_no_ca_with_optin_is_insecure_not_refused(void) {
  TEST_ASSERT_EQUAL(OTA_TLS_INSECURE, otaTlsMode(false, true));
}

// ---- Manifest validation -------------------------------------------------

void test_complete_manifest_is_valid(void) {
  TEST_ASSERT_TRUE(otaManifestValid("26.09.1", "https://h/f.bin", SHA_OK));
}

void test_each_missing_field_invalidates(void) {
  TEST_ASSERT_FALSE(otaManifestValid("", "https://h/f.bin", SHA_OK));
  TEST_ASSERT_FALSE(otaManifestValid("26.09.1", "", SHA_OK));
  TEST_ASSERT_FALSE(otaManifestValid("26.09.1", "https://h/f.bin", ""));
}

void test_null_fields_invalidate(void) {
  TEST_ASSERT_FALSE(otaManifestValid(nullptr, "u", SHA_OK));
  TEST_ASSERT_FALSE(otaManifestValid("v", nullptr, SHA_OK));
  TEST_ASSERT_FALSE(otaManifestValid("v", "u", nullptr));
}

void test_manifest_cap_boundary(void) {
  TEST_ASSERT_TRUE(otaManifestSizeOk(OTA_MANIFEST_CAP - 1, 1));
  TEST_ASSERT_FALSE(otaManifestSizeOk(OTA_MANIFEST_CAP, 1));
  TEST_ASSERT_FALSE(otaManifestSizeOk(OTA_MANIFEST_CAP - 1, 2));
}

// ---- Version predicate ---------------------------------------------------

void test_newer_across_each_field(void) {
  TEST_ASSERT_TRUE(otaIsNewer("2.0.0", "1.9.9"));
  TEST_ASSERT_TRUE(otaIsNewer("1.10.0", "1.9.9"));
  TEST_ASSERT_TRUE(otaIsNewer("1.9.10", "1.9.9"));
}

void test_same_and_older_are_not_newer(void) {
  TEST_ASSERT_FALSE(otaIsNewer("1.9.9", "1.9.9"));
  TEST_ASSERT_FALSE(otaIsNewer("1.9.8", "1.9.9"));
  TEST_ASSERT_FALSE(otaIsNewer("0.9.9", "1.0.0"));
}

// Calendar versions (26.08.2) must order correctly - the fleet uses these.
void test_calver_orders_correctly(void) {
  TEST_ASSERT_TRUE(otaIsNewer("26.09.1", "26.08.2"));
  TEST_ASSERT_FALSE(otaIsNewer("26.08.2", "26.09.1"));
}

// Documents the sscanf weakness deliberately: garbage parses as 0.0.0, so it is
// never "newer" and the !force path refuses it. Safe direction, but not a
// rejection - if this ever flips to true, the guard has been lost.
void test_malformed_remote_is_not_newer(void) {
  TEST_ASSERT_FALSE(otaIsNewer("garbage", "1.0.0"));
  TEST_ASSERT_FALSE(otaIsNewer("", "1.0.0"));
}

// ...and the same weakness the other way: a malformed LOCAL reads 0.0.0, so any
// real remote looks newer. Only reachable if FIRMWARE_VERSION is malformed.
void test_malformed_local_makes_remote_look_newer(void) {
  TEST_ASSERT_TRUE(otaIsNewer("1.0.0", "garbage"));
}

void test_null_versions_are_not_newer(void) {
  TEST_ASSERT_FALSE(otaIsNewer(nullptr, "1.0.0"));
  TEST_ASSERT_FALSE(otaIsNewer("1.0.0", nullptr));
}

// force re-flashes regardless; without it the version gate decides.
void test_force_bypasses_version_gate(void) {
  TEST_ASSERT_TRUE(otaShouldUpdate("1.0.0", "1.0.0", true));
  TEST_ASSERT_FALSE(otaShouldUpdate("1.0.0", "1.0.0", false));
  TEST_ASSERT_TRUE(otaShouldUpdate("1.0.1", "1.0.0", false));
}

// ---- Digest compare ------------------------------------------------------

void test_matching_digest_accepts_either_case(void) {
  TEST_ASSERT_TRUE(otaShaMatches(SHA_OK, SHA_OK));
  TEST_ASSERT_TRUE(otaShaMatches(SHA_OK, SHA_OK_UPPER));
}

void test_mismatched_digest_rejects(void) {
  TEST_ASSERT_FALSE(otaShaMatches(SHA_OK, SHA_OTHER));
}

// A short digest must be rejected on length, not merely fail the compare.
void test_wrong_length_digest_rejects(void) {
  TEST_ASSERT_FALSE(otaShaMatches(SHA_SHORT, SHA_OK));
  TEST_ASSERT_FALSE(otaShaMatches(SHA_OK, SHA_SHORT));
  TEST_ASSERT_FALSE(otaShaMatches("", SHA_OK));
  TEST_ASSERT_FALSE(otaShaMatches(SHA_OK, ""));
}

void test_null_digest_rejects(void) {
  TEST_ASSERT_FALSE(otaShaMatches(nullptr, SHA_OK));
  TEST_ASSERT_FALSE(otaShaMatches(SHA_OK, nullptr));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ca_present_is_verified);
  RUN_TEST(test_no_ca_without_optin_refuses);
  RUN_TEST(test_no_ca_with_optin_is_insecure_not_refused);
  RUN_TEST(test_complete_manifest_is_valid);
  RUN_TEST(test_each_missing_field_invalidates);
  RUN_TEST(test_null_fields_invalidate);
  RUN_TEST(test_manifest_cap_boundary);
  RUN_TEST(test_newer_across_each_field);
  RUN_TEST(test_same_and_older_are_not_newer);
  RUN_TEST(test_calver_orders_correctly);
  RUN_TEST(test_malformed_remote_is_not_newer);
  RUN_TEST(test_malformed_local_makes_remote_look_newer);
  RUN_TEST(test_null_versions_are_not_newer);
  RUN_TEST(test_force_bypasses_version_gate);
  RUN_TEST(test_matching_digest_accepts_either_case);
  RUN_TEST(test_mismatched_digest_rejects);
  RUN_TEST(test_wrong_length_digest_rejects);
  RUN_TEST(test_null_digest_rejects);
  return UNITY_END();
}
