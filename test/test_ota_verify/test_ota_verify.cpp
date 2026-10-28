// Host-native unit tests for ota_verify_policy.h - the OTA verification path
// decisions: TLS insecure guard, manifest validation, version predicate, the
// fled-release skip and its expiry, digest compare.
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

// Malformed remote never counts as newer.
void test_malformed_remote_is_not_newer(void) {
  TEST_ASSERT_FALSE(otaIsNewer("garbage", "1.0.0"));
  TEST_ASSERT_FALSE(otaIsNewer("", "1.0.0"));
  TEST_ASSERT_FALSE(otaIsNewer("1.2", "1.0.0"));
  TEST_ASSERT_FALSE(otaIsNewer("1.2.3x", "1.0.0"));
  TEST_ASSERT_FALSE(otaIsNewer("-1.0.0", "0.0.0"));
}

// Malformed local also refuses (safe): previously sscanf zeroed it and any
// real remote looked newer.
void test_malformed_local_is_not_newer(void) {
  TEST_ASSERT_FALSE(otaIsNewer("1.0.0", "garbage"));
  TEST_ASSERT_FALSE(otaIsNewer("1.0.0", ""));
  TEST_ASSERT_FALSE(otaIsNewer("1.0.0", "1.2"));
  TEST_ASSERT_FALSE(otaIsNewer("1.0.0", "1.2.3x"));
  TEST_ASSERT_FALSE(otaIsNewer("1.0.0", "-1.0.0"));
  TEST_ASSERT_FALSE(otaIsNewer("1.0.0", "2147483648.0.0"));
}

void test_strict_version_parse(void) {
  int maj = -1, min = -1, pat = -1;
  TEST_ASSERT_TRUE(otaParseVersion("26.08.2", &maj, &min, &pat));
  TEST_ASSERT_EQUAL(26, maj);
  TEST_ASSERT_EQUAL(8, min);
  TEST_ASSERT_EQUAL(2, pat);
  TEST_ASSERT_FALSE(otaParseVersion("1.2", &maj, &min, &pat));
  TEST_ASSERT_FALSE(otaParseVersion("1.2.3x", &maj, &min, &pat));
  TEST_ASSERT_FALSE(otaParseVersion("-1.0.0", &maj, &min, &pat));
  TEST_ASSERT_FALSE(otaParseVersion("", &maj, &min, &pat));
  TEST_ASSERT_FALSE(otaParseVersion("1..2", &maj, &min, &pat));
  // Overflow beyond INT_MAX
  TEST_ASSERT_FALSE(otaParseVersion("2147483648.0.0", &maj, &min, &pat));
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
  // force still wins when the local string would fail a strict parse
  TEST_ASSERT_TRUE(otaShouldUpdate("1.0.0", "garbage", true));
  TEST_ASSERT_FALSE(otaShouldUpdate("1.0.0", "garbage", false));
}

// ---- Fled-release skip ---------------------------------------------------

static const char* FLED = "26.10.4";
static const char* RUNNING = "26.10.3";
static const uint32_t T0 = 1800000000u;  // a synced clock
static const uint32_t YEAR_S = 365u * 86400u;

// The image a switch booted refuses the build it fled, which is newer.
void test_fled_release_is_skipped(void) {
  TEST_ASSERT_TRUE(otaShouldUpdate(FLED, RUNNING, false));
  TEST_ASSERT_EQUAL(OTA_FLED_SKIP, otaFledAction(FLED, FLED, 0, 0, false));
  // Read as N.N.N like otaIsNewer, so a zero-padded manifest is the same release.
  TEST_ASSERT_EQUAL(OTA_FLED_SKIP, otaFledAction("26.10.04", FLED, 0, 0, false));
  // Negative control: no record skips nothing.
  TEST_ASSERT_EQUAL(OTA_FLED_NONE, otaFledAction(FLED, "", 0, 0, false));
}

// A newer (or any other) version installs at any age.
void test_another_release_is_not_skipped(void) {
  TEST_ASSERT_EQUAL(OTA_FLED_NONE, otaFledAction("26.10.5", FLED, 0, 0, false));
  TEST_ASSERT_EQUAL(OTA_FLED_NONE, otaFledAction("26.11.4", FLED, 0, 0, false));
  TEST_ASSERT_EQUAL(OTA_FLED_NONE, otaFledAction("27.10.4", FLED, 0, 0, false));
  TEST_ASSERT_EQUAL(OTA_FLED_NONE, otaFledAction("26.10.3", FLED, 0, 0, false));
  // Negative control: the fled one at the same age is refused.
  TEST_ASSERT_EQUAL(OTA_FLED_SKIP, otaFledAction(FLED, FLED, 0, 0, false));
}

// Force installs it at any age and does not spend a try.
void test_force_installs_the_fled_release(void) {
  TEST_ASSERT_EQUAL(OTA_FLED_FORCED, otaFledAction(FLED, FLED, 0, 0, true));
  TEST_ASSERT_EQUAL(OTA_FLED_FORCED, otaFledAction(FLED, FLED, OTA_FLED_SKIP_S - 1, 0, true));
  TEST_ASSERT_EQUAL(OTA_FLED_FORCED, otaFledAction(FLED, FLED, OTA_FLED_SKIP_S, 1, true));
  TEST_ASSERT_TRUE(otaShouldUpdate(FLED, RUNNING, true));
  // Negative control: the same inputs unforced skip.
  TEST_ASSERT_EQUAL(OTA_FLED_SKIP, otaFledAction(FLED, FLED, 0, 0, false));
}

// A record that does not parse must not block the update it cannot name.
void test_unparseable_fled_or_remote_skips_nothing(void) {
  TEST_ASSERT_EQUAL(OTA_FLED_NONE, otaFledAction(FLED, nullptr, 0, 0, false));
  TEST_ASSERT_EQUAL(OTA_FLED_NONE, otaFledAction(FLED, "26.10", 0, 0, false));
  TEST_ASSERT_EQUAL(OTA_FLED_NONE, otaFledAction(FLED, "26.10.4x", 0, 0, false));
  TEST_ASSERT_EQUAL(OTA_FLED_NONE, otaFledAction(nullptr, FLED, 0, 0, false));
  TEST_ASSERT_EQUAL(OTA_FLED_NONE, otaFledAction("garbage", "garbage", 0, 0, false));
  TEST_ASSERT_FALSE(otaSameVersion("garbage", "garbage"));
  TEST_ASSERT_TRUE(otaSameVersion("26.10.04", FLED));
}

void test_fled_skip_expires_at_24h(void) {
  TEST_ASSERT_EQUAL(OTA_FLED_SKIP, otaFledAction(FLED, FLED, OTA_FLED_SKIP_S - 1, 0, false));
  TEST_ASSERT_EQUAL(OTA_FLED_RETRY, otaFledAction(FLED, FLED, OTA_FLED_SKIP_S, 0, false));
  // Saves 10 min apart from a fresh record: refused for 143, installed at 144.
  uint32_t age = 0;
  int saves = 0;
  while (otaFledAction(FLED, FLED, age, 0, false) == OTA_FLED_SKIP && saves < 1000) {
    age = otaFledAdvance(age, otaFledElapsed(OTA_FLED_SAVE_S, false, T0, T0 + YEAR_S));
    saves++;
  }
  TEST_ASSERT_EQUAL(144, saves);
}

// The guard rolls back the first try when the running image hands it a crash
// streak, so one more try, then the day starts over.
void test_fled_second_try_restarts_the_day(void) {
  TEST_ASSERT_EQUAL(OTA_FLED_RETRY, otaFledAction(FLED, FLED, OTA_FLED_SKIP_S, 0, false));
  TEST_ASSERT_EQUAL(OTA_FLED_LAST, otaFledAction(FLED, FLED, OTA_FLED_SKIP_S, 1, false));
  TEST_ASSERT_EQUAL(OTA_FLED_LAST, otaFledAction(FLED, FLED, OTA_FLED_SKIP_S, 200, false));
  // Negative control: an unexpired skip refuses whatever the tries.
  TEST_ASSERT_EQUAL(OTA_FLED_SKIP, otaFledAction(FLED, FLED, OTA_FLED_SKIP_S - 1, 1, false));
}

// Every save lost: each boot reads the expired record with no try spent.
void test_fled_try_downloads_only_once_saved(void) {
  TEST_ASSERT_FALSE(otaFledMayDownload(OTA_FLED_RETRY, false));
  TEST_ASSERT_FALSE(otaFledMayDownload(OTA_FLED_LAST, false));
  TEST_ASSERT_TRUE(otaFledMayDownload(OTA_FLED_RETRY, true));
  TEST_ASSERT_TRUE(otaFledMayDownload(OTA_FLED_LAST, true));
  // Negative control: an install that spends no try never waits on a save.
  TEST_ASSERT_TRUE(otaFledMayDownload(OTA_FLED_NONE, false));
  TEST_ASSERT_TRUE(otaFledMayDownload(OTA_FLED_FORCED, false));
  int downloads = 0;
  for (int boot = 0; boot < 10; boot++) {
    const OtaFledAction a = otaFledAction(FLED, FLED, OTA_FLED_SKIP_S, 0, false);
    if (otaFledMayDownload(a, false)) downloads++;
  }
  TEST_ASSERT_EQUAL(0, downloads);
}

// Checks in one boot after the day: the first try's save fails, then NVS works.
void test_fled_failed_try_save_spends_no_try(void) {
  uint32_t age = OTA_FLED_SKIP_S;
  uint8_t tries = 0;
  int installs = 0;
  const bool saves[] = {false, true, true};
  for (bool ok : saves) {
    const OtaFledAction a = otaFledAction(FLED, FLED, age, tries, false);
    if (otaFledMayDownload(a, ok)) installs++;
    const OtaFledAge held = otaFledAfterTry(a, age, tries, ok);
    age = held.ageS;
    tries = held.tries;
    if (!ok) {
      TEST_ASSERT_EQUAL(OTA_FLED_RETRY, otaFledAction(FLED, FLED, age, tries, false));
      // The held age stays at the cap, so ageFledRelease writes nothing.
      TEST_ASSERT_FALSE(otaFledAges(FLED, RUNNING, age));
    }
  }
  TEST_ASSERT_EQUAL(2, installs);
  // Negative control: two good saves spend the day.
  TEST_ASSERT_EQUAL(OTA_FLED_SKIP, otaFledAction(FLED, FLED, age, tries, false));
  const OtaFledAge last = otaFledAfterTry(OTA_FLED_LAST, OTA_FLED_SKIP_S, 1, false);
  TEST_ASSERT_EQUAL_UINT32(OTA_FLED_SKIP_S, last.ageS);
  TEST_ASSERT_EQUAL_UINT8(1, last.tries);
}

void test_fled_age_survives_reboot(void) {
  const OtaFledAge saved = {OTA_FLED_SKIP_S - 600u, 1, T0};
  const OtaFledAge back = otaFledUnpack(otaFledPack(saved));
  TEST_ASSERT_EQUAL_UINT32(saved.ageS, back.ageS);
  TEST_ASSERT_EQUAL_UINT8(saved.tries, back.tries);
  TEST_ASSERT_EQUAL_UINT32(saved.seenEpoch, back.seenEpoch);
  // No fled_age key yet reads as a fresh day.
  const OtaFledAge fresh = otaFledUnpack(0);
  TEST_ASSERT_EQUAL_UINT32(0, fresh.ageS);
  TEST_ASSERT_EQUAL_UINT8(0, fresh.tries);
  TEST_ASSERT_EQUAL_UINT32(0, fresh.seenEpoch);
  // An age past the 24 bits is held at the skip, not wrapped to young.
  TEST_ASSERT_EQUAL_UINT32(OTA_FLED_SKIP_S, otaFledUnpack(otaFledPack({0xFFFFFFFFu, 0, 0})).ageS);
  // A crash: 10 min short, 5 min carried in RTC memory, 5 min up reach 24 h.
  uint32_t carry = otaFledCarry(300u, ~300u);
  TEST_ASSERT_EQUAL_UINT32(300u, carry);
  uint32_t age = otaFledAdvance(back.ageS, otaFledElapsed(carry + 300u, true, 0, 0));
  TEST_ASSERT_EQUAL(OTA_FLED_RETRY, otaFledAction(FLED, FLED, age, 0, false));
  // Negative control: a power-on leaves noise, the tail is lost, still refused.
  carry = otaFledCarry(300u, 300u);
  TEST_ASSERT_EQUAL_UINT32(0, carry);
  age = otaFledAdvance(back.ageS, otaFledElapsed(carry + 300u, true, 0, 0));
  TEST_ASSERT_EQUAL(OTA_FLED_SKIP, otaFledAction(FLED, FLED, age, 0, false));
}

void test_fled_carry_is_capped(void) {
  TEST_ASSERT_EQUAL_UINT32(2u * OTA_FLED_SAVE_S, otaFledCarry(1000000u, ~1000000u));
  TEST_ASSERT_EQUAL_UINT32(0, otaFledCarry(0, ~0u));
}

// Without a synced clock on both saves only uptime counts.
void test_fled_unsynced_clock_counts_uptime(void) {
  TEST_ASSERT_EQUAL_UINT32(600u, otaFledElapsed(600u, true, 0, T0 + 5000u));
  TEST_ASSERT_EQUAL_UINT32(600u, otaFledElapsed(600u, true, T0, 0));
  // Negative control: synced on both, 30 min off is credited.
  TEST_ASSERT_EQUAL_UINT32(600u + 1800u, otaFledElapsed(600u, true, T0, T0 + 600u + 1800u));
}

// An NTP step or a set clock inside one boot moves nothing.
void test_fled_clock_reads_only_across_a_reset(void) {
  TEST_ASSERT_EQUAL_UINT32(600u, otaFledElapsed(600u, false, T0, T0 + YEAR_S));
  // Negative control: the same reading on a boot's first save adds the capped gap.
  TEST_ASSERT_EQUAL_UINT32(600u + OTA_FLED_GAP_MAX_S, otaFledElapsed(600u, true, T0, T0 + YEAR_S));
}

void test_fled_clock_jump_backwards(void) {
  TEST_ASSERT_EQUAL_UINT32(600u, otaFledElapsed(600u, true, T0, T0 - YEAR_S));
  TEST_ASSERT_EQUAL_UINT32(600u, otaFledElapsed(600u, true, T0, T0));
  // Not permanent: with the clock a year behind every save, uptime still lifts it at 24 h.
  uint32_t age = 0;
  int saves = 0;
  while (otaFledAction(FLED, FLED, age, 0, false) == OTA_FLED_SKIP && saves < 1000) {
    age = otaFledAdvance(age, otaFledElapsed(OTA_FLED_SAVE_S, true, T0, T0 - YEAR_S));
    saves++;
  }
  TEST_ASSERT_EQUAL(144, saves);
}

void test_fled_clock_jump_forward_is_bounded(void) {
  // A year ahead adds one hour at most, so 22 h saved stays refused.
  uint32_t age = otaFledAdvance(22u * 3600u, otaFledElapsed(60u, true, T0, T0 + YEAR_S));
  TEST_ASSERT_EQUAL_UINT32(22u * 3600u + 60u + OTA_FLED_GAP_MAX_S, age);
  TEST_ASSERT_EQUAL(OTA_FLED_SKIP, otaFledAction(FLED, FLED, age, 0, false));
  // Negative control: a gap under the cap is credited whole.
  TEST_ASSERT_EQUAL_UINT32(60u + 1200u, otaFledElapsed(60u, true, T0, T0 + 60u + 1200u));
  // A clock gap shorter than the uptime it spans adds nothing.
  TEST_ASSERT_EQUAL_UINT32(600u, otaFledElapsed(600u, true, T0, T0 + 300u));
}

void test_fled_age_holds_at_the_skip(void) {
  TEST_ASSERT_EQUAL_UINT32(150u, otaFledAdvance(100u, 50u));
  TEST_ASSERT_EQUAL_UINT32(OTA_FLED_SKIP_S, otaFledAdvance(OTA_FLED_SKIP_S - 1, 5u));
  TEST_ASSERT_EQUAL_UINT32(OTA_FLED_SKIP_S, otaFledAdvance(OTA_FLED_SKIP_S, 0));
  TEST_ASSERT_EQUAL_UINT32(OTA_FLED_SKIP_S, otaFledAdvance(OTA_FLED_SKIP_S + 5u, 0));
  TEST_ASSERT_EQUAL_UINT32(OTA_FLED_SKIP_S, otaFledAdvance(100u, 0xFFFFFFFFu));
}

// A check right after a save writes nothing; a boot's first one always does.
void test_fled_save_is_paced(void) {
  TEST_ASSERT_TRUE(otaFledSaveDue(0, true));
  TEST_ASSERT_FALSE(otaFledSaveDue(OTA_FLED_SAVE_S - 1, false));
  // Negative control: OTA_FLED_SAVE_S unsaved is due.
  TEST_ASSERT_TRUE(otaFledSaveDue(OTA_FLED_SAVE_S, false));
}

// Boots of onS s, 10 s off, the boot check at 15 s up, a turn a second, RTC
// memory noise after each power-on. out: boots until the counted age is 24 h.
static int bootsToDay(uint32_t onS, bool synced) {
  uint32_t ageS = 0, seen = 0, now = T0;
  for (int boot = 1; boot <= 20000; boot++) {
    uint32_t carry = otaFledCarry(0x5A5A5A5Au, 0x5A5A5A5Au), mark = 0;
    bool first = true;
    for (uint32_t up = 15u; up < onS; up++) {
      if (!otaFledSaveDue(carry + up - mark, first)) continue;
      const uint32_t epoch = synced ? now + up : 0u;
      ageS = otaFledAdvance(ageS, otaFledElapsed(carry + up - mark, first, seen, epoch));
      if (ageS >= OTA_FLED_SKIP_S) return boot;
      seen = epoch;
      carry = 0;
      mark = up;
      first = false;
    }
    now += onS + 10u;
  }
  return -1;
}

// No clock: a power-on loses under 10 min, but a boot shorter than that counts
// only the 15 s before its first check, one save a boot.
void test_fled_power_cycles_without_clock(void) {
  TEST_ASSERT_EQUAL(36, bootsToDay(50u * 60u - 10u, false));
  TEST_ASSERT_EQUAL(5760, bootsToDay(5u * 60u - 10u, false));
  // Negative control: a synced clock credits the time off, about a day of boots.
  TEST_ASSERT_EQUAL(289, bootsToDay(5u * 60u - 10u, true));
}

// No saves on the fled build itself, or once the skip has expired.
void test_fled_ages_only_while_it_can_refuse(void) {
  TEST_ASSERT_TRUE(otaFledAges(FLED, RUNNING, 0));
  TEST_ASSERT_TRUE(otaFledAges(FLED, RUNNING, OTA_FLED_SKIP_S - 1));
  TEST_ASSERT_TRUE(otaFledAges(FLED, "dev", 0));
  TEST_ASSERT_FALSE(otaFledAges(FLED, RUNNING, OTA_FLED_SKIP_S));
  TEST_ASSERT_FALSE(otaFledAges(FLED, "26.10.04", 0));
  TEST_ASSERT_FALSE(otaFledAges("", RUNNING, 0));
  TEST_ASSERT_FALSE(otaFledAges(nullptr, RUNNING, 0));
  TEST_ASSERT_FALSE(otaFledAges("garbage", RUNNING, 0));
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

void test_redirect_codes(void) {
  const int yes[] = {301, 302, 303, 307, 308};
  for (int c : yes) TEST_ASSERT_TRUE(otaIsRedirect(c));
  const int no[] = {200, 204, 300, 304, 305, 306, 400, 404, -1, 0};
  for (int c : no) TEST_ASSERT_FALSE(otaIsRedirect(c));
}

void test_redirect_keeps_scheme(void) {
  TEST_ASSERT_TRUE(otaRedirectAllowed("https://a.example/f", "https://b.example/g"));
  TEST_ASSERT_TRUE(otaRedirectAllowed("http://a.example/f", "http://b.example/g"));
  TEST_ASSERT_FALSE(otaRedirectAllowed("https://a.example/f", "http://b.example/g"));
  TEST_ASSERT_FALSE(otaRedirectAllowed("http://a.example/f", "https://b.example/g"));
}

void test_redirect_refuses_relative_and_junk(void) {
  TEST_ASSERT_FALSE(otaRedirectAllowed("https://a.example/f", "/g"));
  TEST_ASSERT_FALSE(otaRedirectAllowed("https://a.example/f", "https://"));
  TEST_ASSERT_FALSE(otaRedirectAllowed("https://a.example/f", "https:///g"));
  TEST_ASSERT_FALSE(otaRedirectAllowed("ftp://a.example/f", "ftp://b.example/g"));
  TEST_ASSERT_FALSE(otaRedirectAllowed(nullptr, "https://b.example/g"));
  TEST_ASSERT_FALSE(otaRedirectAllowed("https://a.example/f", nullptr));
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
  RUN_TEST(test_malformed_local_is_not_newer);
  RUN_TEST(test_strict_version_parse);
  RUN_TEST(test_null_versions_are_not_newer);
  RUN_TEST(test_force_bypasses_version_gate);
  RUN_TEST(test_fled_release_is_skipped);
  RUN_TEST(test_another_release_is_not_skipped);
  RUN_TEST(test_force_installs_the_fled_release);
  RUN_TEST(test_unparseable_fled_or_remote_skips_nothing);
  RUN_TEST(test_fled_skip_expires_at_24h);
  RUN_TEST(test_fled_second_try_restarts_the_day);
  RUN_TEST(test_fled_try_downloads_only_once_saved);
  RUN_TEST(test_fled_failed_try_save_spends_no_try);
  RUN_TEST(test_fled_age_survives_reboot);
  RUN_TEST(test_fled_carry_is_capped);
  RUN_TEST(test_fled_unsynced_clock_counts_uptime);
  RUN_TEST(test_fled_clock_reads_only_across_a_reset);
  RUN_TEST(test_fled_clock_jump_backwards);
  RUN_TEST(test_fled_clock_jump_forward_is_bounded);
  RUN_TEST(test_fled_age_holds_at_the_skip);
  RUN_TEST(test_fled_ages_only_while_it_can_refuse);
  RUN_TEST(test_fled_save_is_paced);
  RUN_TEST(test_fled_power_cycles_without_clock);
  RUN_TEST(test_matching_digest_accepts_either_case);
  RUN_TEST(test_mismatched_digest_rejects);
  RUN_TEST(test_wrong_length_digest_rejects);
  RUN_TEST(test_null_digest_rejects);
  RUN_TEST(test_redirect_codes);
  RUN_TEST(test_redirect_keeps_scheme);
  RUN_TEST(test_redirect_refuses_relative_and_junk);
  return UNITY_END();
}
