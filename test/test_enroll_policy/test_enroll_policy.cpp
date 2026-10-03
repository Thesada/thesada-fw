// Host-native unit tests for enroll_policy.h (first-boot enrollment).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "enroll_policy.h"

void setUp(void) {}
void tearDown(void) {}

static const char* kId  = "thesada-dcb4d91acd28";
static const char* kPk  = "8f3a1c0d9e2b4f6a7c5e1d3b9a0f2e4c6b8d0a1f3e5c7b9d2f4a6c8e0b1d3f5a";
static const char* kSig =
  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

// --- enrollHexValid ---------------------------------------------------------

void test_hex_accepts_exact_lowercase(void) {
  TEST_ASSERT_TRUE(enrollHexValid("00ff", 4));
  TEST_ASSERT_TRUE(enrollHexValid(kPk, 64));
}

// The server compares hex as strings, so uppercase would be a different key.
void test_hex_refuses_case_length_and_junk(void) {
  TEST_ASSERT_FALSE(enrollHexValid(nullptr, 4));
  TEST_ASSERT_FALSE(enrollHexValid("00FF", 4));
  TEST_ASSERT_FALSE(enrollHexValid("00f", 4));
  TEST_ASSERT_FALSE(enrollHexValid("00fff", 4));
  TEST_ASSERT_FALSE(enrollHexValid("00fg", 4));
}

// --- enrollClaimCodeValid ---------------------------------------------------

void test_claim_code_is_eight_digits(void) {
  TEST_ASSERT_TRUE(enrollClaimCodeValid("48271935"));
  TEST_ASSERT_TRUE(enrollClaimCodeValid("00000000"));
}

void test_claim_code_refuses_other_shapes(void) {
  TEST_ASSERT_FALSE(enrollClaimCodeValid(nullptr));
  TEST_ASSERT_FALSE(enrollClaimCodeValid(""));
  TEST_ASSERT_FALSE(enrollClaimCodeValid("4827193"));
  TEST_ASSERT_FALSE(enrollClaimCodeValid("482719350"));
  TEST_ASSERT_FALSE(enrollClaimCodeValid("4827 1935"));
  TEST_ASSERT_FALSE(enrollClaimCodeValid("4827193a"));
}

// --- enrollUrlUsable --------------------------------------------------------

void test_url_accepts_https_hosts(void) {
  TEST_ASSERT_TRUE(enrollUrlUsable("https://app.example.com"));
  TEST_ASSERT_TRUE(enrollUrlUsable("https://app.example.com/"));
  TEST_ASSERT_TRUE(enrollUrlUsable("https://example.com:8443/thesada"));
}

// Plain http would hand the claim code and the private key to the network.
void test_url_refuses_non_https(void) {
  TEST_ASSERT_FALSE(enrollUrlUsable(nullptr));
  TEST_ASSERT_FALSE(enrollUrlUsable(""));
  TEST_ASSERT_FALSE(enrollUrlUsable("http://app.example.com"));
  TEST_ASSERT_FALSE(enrollUrlUsable("HTTPS://app.example.com"));
  TEST_ASSERT_FALSE(enrollUrlUsable("ftp://app.example.com"));
}

void test_url_refuses_missing_host(void) {
  TEST_ASSERT_FALSE(enrollUrlUsable("https://"));
  TEST_ASSERT_FALSE(enrollUrlUsable("https:///path"));
  TEST_ASSERT_FALSE(enrollUrlUsable("https://:443"));
}

void test_url_refuses_query_fragment_userinfo_and_controls(void) {
  TEST_ASSERT_FALSE(enrollUrlUsable("https://a.example.com?x=1"));
  TEST_ASSERT_FALSE(enrollUrlUsable("https://a.example.com#top"));
  TEST_ASSERT_FALSE(enrollUrlUsable("https://evil@a.example.com"));
  TEST_ASSERT_FALSE(enrollUrlUsable("https://a.example.com/a b"));
  TEST_ASSERT_FALSE(enrollUrlUsable("https://a.example.com/\t"));
  TEST_ASSERT_FALSE(enrollUrlUsable("https://a.example.com/\x7f"));
}

void test_url_refuses_over_cap(void) {
  char url[ENROLL_URL_CAP + 8];
  memset(url, 'a', sizeof(url));
  memcpy(url, "https://", 8);
  url[ENROLL_URL_CAP] = '\0';
  TEST_ASSERT_FALSE(enrollUrlUsable(url));
  url[ENROLL_URL_CAP - 1] = '\0';
  TEST_ASSERT_TRUE(enrollUrlUsable(url));
}

// --- enrollEndpoint ---------------------------------------------------------

void test_endpoint_joins_path_and_suffix(void) {
  char out[160];
  TEST_ASSERT_TRUE(enrollEndpoint("https://app.example.com", "", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("https://app.example.com/api/v1/devices/enroll", out);
  TEST_ASSERT_TRUE(enrollEndpoint("https://app.example.com", "/cert", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("https://app.example.com/api/v1/devices/enroll/cert", out);
}

// A portal-typed URL often ends in "/"; a doubled slash is a different route.
void test_claim_link_drops_trailing_slashes(void) {
  char out[180];
  TEST_ASSERT_TRUE(enrollClaimLink(out, sizeof(out), "https://example.com/", kId, "12345678"));
  TEST_ASSERT_EQUAL_STRING(
      "https://example.com/devices/claim?device_id=thesada-dcb4d91acd28&code=12345678", out);
  TEST_ASSERT_TRUE(enrollClaimLink(out, sizeof(out), "https://example.com//", kId, "12345678"));
  TEST_ASSERT_EQUAL_STRING(
      "https://example.com/devices/claim?device_id=thesada-dcb4d91acd28&code=12345678", out);
  TEST_ASSERT_FALSE(enrollClaimLink(out, sizeof(out), "https://example.com", kId, "1234567"));
  TEST_ASSERT_EQUAL_STRING("", out);
}

void test_endpoint_drops_trailing_slashes(void) {
  char out[160];
  TEST_ASSERT_TRUE(enrollEndpoint("https://example.com/sub//", "/ack", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("https://example.com/sub/api/v1/devices/enroll/ack", out);
}

void test_endpoint_refuses_bad_input_and_truncation(void) {
  char out[160];
  TEST_ASSERT_FALSE(enrollEndpoint("http://example.com", "", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("", out);
  TEST_ASSERT_FALSE(enrollEndpoint("https://example.com", nullptr, out, sizeof(out)));
  TEST_ASSERT_FALSE(enrollEndpoint("https://example.com", "", nullptr, 16));
  TEST_ASSERT_FALSE(enrollEndpoint("https://example.com", "", out, 0));
  char small[24];
  TEST_ASSERT_FALSE(enrollEndpoint("https://example.com", "", small, sizeof(small)));
  TEST_ASSERT_EQUAL_STRING("", small);
}

// --- enrollBody -------------------------------------------------------------

void test_body_with_claim_token(void) {
  char out[256];
  TEST_ASSERT_TRUE(enrollBody(out, sizeof(out), kId, kPk, "claim_token", "48271935"));
  char want[256];
  (void)snprintf(want, sizeof(want),
                 "{\"device_id\":\"%s\",\"pubkey\":\"%s\",\"claim_token\":\"48271935\"}", kId, kPk);
  TEST_ASSERT_EQUAL_STRING(want, out);
}

void test_body_with_signature(void) {
  char out[320];
  TEST_ASSERT_TRUE(enrollBody(out, sizeof(out), kId, kPk, "signature", kSig));
  TEST_ASSERT_NOT_NULL(strstr(out, "\"signature\":\"0123456789abcdef"));
}

// Values go into JSON unescaped, so anything off-shape must never reach snprintf.
void test_body_refuses_off_shape_values(void) {
  char out[320];
  TEST_ASSERT_FALSE(enrollBody(out, sizeof(out), "thesada-node", kPk, "claim_token", "48271935"));
  TEST_ASSERT_EQUAL_STRING("", out);
  TEST_ASSERT_FALSE(enrollBody(out, sizeof(out), kId, "abc", "claim_token", "48271935"));
  TEST_ASSERT_FALSE(enrollBody(out, sizeof(out), kId, kPk, "claim_token", "4827\"}"));
  TEST_ASSERT_FALSE(enrollBody(out, sizeof(out), kId, kPk, "signature", "48271935"));
  TEST_ASSERT_FALSE(enrollBody(out, sizeof(out), kId, kPk, "claim_token", kSig));
  TEST_ASSERT_FALSE(enrollBody(out, sizeof(out), kId, kPk, "tenant", "48271935"));
  TEST_ASSERT_FALSE(enrollBody(out, sizeof(out), kId, kPk, nullptr, "48271935"));
  TEST_ASSERT_FALSE(enrollBody(out, sizeof(out), kId, kPk, "claim_token", nullptr));
  TEST_ASSERT_FALSE(enrollBody(nullptr, 64, kId, kPk, "claim_token", "48271935"));
  TEST_ASSERT_FALSE(enrollBody(out, 0, kId, kPk, "claim_token", "48271935"));
}

void test_body_refuses_truncation(void) {
  char small[64];
  TEST_ASSERT_FALSE(enrollBody(small, sizeof(small), kId, kPk, "claim_token", "48271935"));
  TEST_ASSERT_EQUAL_STRING("", small);
}

// --- enrollBackoffMs --------------------------------------------------------

// Never below one request per 120 s, the server's per-device cap.
void test_backoff_starts_above_server_rate_cap(void) {
  TEST_ASSERT_EQUAL_UINT32(ENROLL_BACKOFF_MIN_MS, enrollBackoffMs(0, 0));
  TEST_ASSERT_TRUE(ENROLL_BACKOFF_MIN_MS > 120000u);
  TEST_ASSERT_TRUE(ENROLL_POLL_MS > 120000u);
}

void test_backoff_doubles_then_caps(void) {
  TEST_ASSERT_EQUAL_UINT32(ENROLL_BACKOFF_MIN_MS * 2, enrollBackoffMs(1, 0));
  TEST_ASSERT_EQUAL_UINT32(ENROLL_BACKOFF_MIN_MS * 4, enrollBackoffMs(2, 0));
  TEST_ASSERT_EQUAL_UINT32(ENROLL_BACKOFF_MAX_MS, enrollBackoffMs(10, 0));
  TEST_ASSERT_EQUAL_UINT32(ENROLL_BACKOFF_MAX_MS, enrollBackoffMs(255, 0));
}

void test_backoff_jitter_stays_within_a_quarter(void) {
  uint32_t base = enrollBackoffMs(0, 0);
  TEST_ASSERT_EQUAL_UINT32(base + 7, enrollBackoffMs(0, 7));
  uint32_t top = enrollBackoffMs(0, 0xFFFFFFFFu);
  TEST_ASSERT_TRUE(top >= base && top <= base + base / 4);
}

// --- enrollStartStep --------------------------------------------------------

void test_boot_announces_until_a_cert_is_stored(void) {
  TEST_ASSERT_EQUAL(EnrollStep::Announce, enrollStartStep(false, false));
  TEST_ASSERT_EQUAL(EnrollStep::Announce, enrollStartStep(false, true));
  TEST_ASSERT_EQUAL(EnrollStep::AwaitMtls, enrollStartStep(true, true));
  TEST_ASSERT_EQUAL(EnrollStep::Done, enrollStartStep(true, false));
}

// --- enrollOnResponse -------------------------------------------------------

static void expect(EnrollAction a, EnrollStep next, uint32_t delay, bool progressed) {
  TEST_ASSERT_EQUAL(next, a.next);
  TEST_ASSERT_EQUAL_UINT32(delay, a.delayMs);
  TEST_ASSERT_EQUAL(progressed, a.progressed);
}

void test_announce_moves_to_verify_only_on_200(void) {
  uint32_t back = enrollBackoffMs(0, 0);
  expect(enrollOnResponse(EnrollStep::Announce, 200, 0, 0), EnrollStep::Verify, 0, true);
  expect(enrollOnResponse(EnrollStep::Announce, 403, 0, 0), EnrollStep::Announce, back, false);
  expect(enrollOnResponse(EnrollStep::Announce, -1, 0, 0), EnrollStep::Announce, back, false);
}

// A failed verify burned the challenge; retrying verify can never succeed.
void test_verify_failure_reannounces(void) {
  uint32_t back = enrollBackoffMs(1, 0);
  expect(enrollOnResponse(EnrollStep::Verify, 200, 1, 0), EnrollStep::Poll, 0, true);
  expect(enrollOnResponse(EnrollStep::Verify, 403, 1, 0), EnrollStep::Announce, back, false);
  expect(enrollOnResponse(EnrollStep::Verify, 500, 1, 0), EnrollStep::Announce, back, false);
}

void test_poll_waits_on_204_and_stores_on_200(void) {
  uint32_t back = enrollBackoffMs(2, 0);
  expect(enrollOnResponse(EnrollStep::Poll, 204, 2, 0), EnrollStep::Poll, ENROLL_POLL_MS, true);
  expect(enrollOnResponse(EnrollStep::Poll, 200, 2, 0), EnrollStep::AwaitMtls, 0, true);
  expect(enrollOnResponse(EnrollStep::Poll, 403, 2, 0), EnrollStep::Announce, back, false);
  expect(enrollOnResponse(EnrollStep::Poll, 500, 2, 0), EnrollStep::Poll, back, false);
  expect(enrollOnResponse(EnrollStep::Poll, -1, 2, 0), EnrollStep::Poll, back, false);
}

void test_ack_seals_only_on_200(void) {
  uint32_t back = enrollBackoffMs(0, 0);
  expect(enrollOnResponse(EnrollStep::Ack, 200, 0, 0), EnrollStep::Done, 0, true);
  expect(enrollOnResponse(EnrollStep::Ack, 403, 0, 0), EnrollStep::Ack, back, false);
  expect(enrollOnResponse(EnrollStep::Ack, -1, 0, 0), EnrollStep::Ack, back, false);
}

// Steps without an HTTP call ignore answers rather than jumping anywhere.
void test_non_http_steps_hold(void) {
  expect(enrollOnResponse(EnrollStep::AwaitMtls, 200, 0, 0), EnrollStep::AwaitMtls, 0, false);
  expect(enrollOnResponse(EnrollStep::Done, 200, 0, 0), EnrollStep::Done, 0, false);
}

// --- cert reply -------------------------------------------------------------

void test_host_accepts_dns_names(void) {
  TEST_ASSERT_TRUE(enrollHostUsable("mqtt.example.com"));
  TEST_ASSERT_TRUE(enrollHostUsable("broker-1.Example.org"));
  TEST_ASSERT_TRUE(enrollHostUsable("192.0.2.10"));
}

void test_host_refuses_junk_and_over_cap(void) {
  TEST_ASSERT_FALSE(enrollHostUsable(nullptr));
  TEST_ASSERT_FALSE(enrollHostUsable(""));
  TEST_ASSERT_FALSE(enrollHostUsable(".example.com"));
  TEST_ASSERT_FALSE(enrollHostUsable("-example.com"));
  TEST_ASSERT_FALSE(enrollHostUsable("mqtt.example.com:8884"));
  TEST_ASSERT_FALSE(enrollHostUsable("mqtt example.com"));
  TEST_ASSERT_FALSE(enrollHostUsable("mqtt/example.com"));
  char host[ENROLL_HOST_CAP + 1];
  memset(host, 'a', sizeof(host) - 1);
  host[ENROLL_HOST_CAP - 1] = '\0';
  TEST_ASSERT_TRUE(enrollHostUsable(host));
  host[ENROLL_HOST_CAP - 1] = 'a';
  host[ENROLL_HOST_CAP] = '\0';
  TEST_ASSERT_FALSE(enrollHostUsable(host));
}

void test_prefix_accepts_tenant_paths(void) {
  TEST_ASSERT_TRUE(enrollPrefixUsable("thesada/acme/thesada-dcb4d91acd28", 128));
  TEST_ASSERT_TRUE(enrollPrefixUsable("p", 128));
}

// "/cmd/config" must still fit: the MQTT config path refuses anything longer.
void test_prefix_refuses_wildcards_edges_and_over_cap(void) {
  TEST_ASSERT_FALSE(enrollPrefixUsable(nullptr, 128));
  TEST_ASSERT_FALSE(enrollPrefixUsable("", 128));
  TEST_ASSERT_FALSE(enrollPrefixUsable("/thesada/acme", 128));
  TEST_ASSERT_FALSE(enrollPrefixUsable("thesada/acme/", 128));
  TEST_ASSERT_FALSE(enrollPrefixUsable("thesada/+/x", 128));
  TEST_ASSERT_FALSE(enrollPrefixUsable("thesada/#", 128));
  TEST_ASSERT_FALSE(enrollPrefixUsable("thesada/a b", 128));
  TEST_ASSERT_TRUE(enrollPrefixUsable("abcd", 16));
  TEST_ASSERT_FALSE(enrollPrefixUsable("abcde", 16));
}

void test_reply_needs_our_id_and_usable_fields(void) {
  const char* pfx = "thesada/acme/thesada-dcb4d91acd28";
  TEST_ASSERT_TRUE(enrollReplyUsable(kId, kId, "mqtt.example.com", 8884, pfx, 128));
  TEST_ASSERT_FALSE(enrollReplyUsable(kId, "thesada-000000000000", "mqtt.example.com", 8884, pfx, 128));
  TEST_ASSERT_FALSE(enrollReplyUsable(kId, nullptr, "mqtt.example.com", 8884, pfx, 128));
  TEST_ASSERT_FALSE(enrollReplyUsable(nullptr, kId, "mqtt.example.com", 8884, pfx, 128));
  TEST_ASSERT_FALSE(enrollReplyUsable(kId, kId, "mqtt.example.com", 0, pfx, 128));
  TEST_ASSERT_FALSE(enrollReplyUsable(kId, kId, "mqtt.example.com", 65536, pfx, 128));
  TEST_ASSERT_FALSE(enrollReplyUsable(kId, kId, "", 8884, pfx, 128));
  TEST_ASSERT_FALSE(enrollReplyUsable(kId, kId, "mqtt.example.com", 8884, "", 128));
}

// --- revocation status check -------------------------------------------------

// The DER serial carries a 00 pad when the top bit is set, and each byte is
// zero-padded; the app stores Go's %x of the integer.
void test_serial_canonical_matches_the_app_form(void) {
  char out[ENROLL_SERIAL_HEX_CAP];
  TEST_ASSERT_TRUE(enrollSerialCanonical("00c3a1", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("c3a1", out);
  TEST_ASSERT_TRUE(enrollSerialCanonical("0a0b", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("a0b", out);
  TEST_ASSERT_TRUE(enrollSerialCanonical("00", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("0", out);
  TEST_ASSERT_TRUE(enrollSerialCanonical("7f", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("7f", out);
}

void test_serial_canonical_refuses_junk_and_overflow(void) {
  char out[ENROLL_SERIAL_HEX_CAP];
  TEST_ASSERT_FALSE(enrollSerialCanonical(nullptr, out, sizeof(out)));
  TEST_ASSERT_FALSE(enrollSerialCanonical("", out, sizeof(out)));
  TEST_ASSERT_FALSE(enrollSerialCanonical("00C3", out, sizeof(out)));
  TEST_ASSERT_FALSE(enrollSerialCanonical("0x1f", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("", out);
  char tiny[3];
  TEST_ASSERT_FALSE(enrollSerialCanonical("00abc", tiny, sizeof(tiny)));
  TEST_ASSERT_EQUAL_STRING("", tiny);
}

void test_serial_valid_is_canonical_only(void) {
  TEST_ASSERT_TRUE(enrollSerialValid("c3a1"));
  TEST_ASSERT_TRUE(enrollSerialValid("0"));
  TEST_ASSERT_FALSE(enrollSerialValid("0c3a1"));
  TEST_ASSERT_FALSE(enrollSerialValid("C3A1"));
  TEST_ASSERT_FALSE(enrollSerialValid(""));
  TEST_ASSERT_FALSE(enrollSerialValid(nullptr));
  TEST_ASSERT_FALSE(enrollSerialValid("1234567890123456789012345678901234567890a"));
}

// The app rebuilds this string from the body, byte for byte.
void test_status_message_is_the_signed_statement(void) {
  char out[160];
  TEST_ASSERT_TRUE(enrollStatusMessage(out, sizeof(out), kId, "c3a1", 1790611200LL));
  TEST_ASSERT_EQUAL_STRING("thesada-enroll-status\nthesada-dcb4d91acd28\nc3a1\n1790611200", out);
}

// A clock still at the build floor would sign a timestamp the app refuses.
void test_status_message_refuses_unset_clock_and_bad_fields(void) {
  char out[160];
  TEST_ASSERT_FALSE(enrollStatusMessage(out, sizeof(out), kId, "c3a1", 86400LL));
  TEST_ASSERT_EQUAL_STRING("", out);
  TEST_ASSERT_FALSE(enrollStatusMessage(out, sizeof(out), "not-an-id", "c3a1", 1790611200LL));
  TEST_ASSERT_FALSE(enrollStatusMessage(out, sizeof(out), kId, "00c3a1", 1790611200LL));
  char tiny[16];
  TEST_ASSERT_FALSE(enrollStatusMessage(tiny, sizeof(tiny), kId, "c3a1", 1790611200LL));
  TEST_ASSERT_EQUAL_STRING("", tiny);
}

void test_status_body_carries_every_field(void) {
  char out[400];
  TEST_ASSERT_TRUE(enrollStatusBody(out, sizeof(out), kId, kPk, "c3a1", 1790611200LL, kSig));
  char want[400];
  int w = snprintf(want, sizeof(want),
                   "{\"device_id\":\"%s\",\"pubkey\":\"%s\",\"serial\":\"c3a1\","
                   "\"ts\":1790611200,\"signature\":\"%s\"}", kId, kPk, kSig);
  TEST_ASSERT_TRUE(w > 0 && (size_t)w < sizeof(want));
  TEST_ASSERT_EQUAL_STRING(want, out);
}

void test_status_body_refuses_off_shape_values(void) {
  char out[400];
  TEST_ASSERT_FALSE(enrollStatusBody(out, sizeof(out), kId, kPk, "c3a1", 1790611200LL, "abcd"));
  TEST_ASSERT_FALSE(enrollStatusBody(out, sizeof(out), kId, "abcd", "c3a1", 1790611200LL, kSig));
  TEST_ASSERT_FALSE(enrollStatusBody(out, sizeof(out), kId, kPk, "", 1790611200LL, kSig));
  TEST_ASSERT_FALSE(enrollStatusBody(out, sizeof(out), kId, kPk, "c3a1", 0LL, kSig));
  char small[64];
  TEST_ASSERT_FALSE(enrollStatusBody(small, sizeof(small), kId, kPk, "c3a1", 1790611200LL, kSig));
  TEST_ASSERT_EQUAL_STRING("", small);
}

// Only an explicit revoked for this serial wipes; an outage or a stale
// answer for another cert keeps it.
void test_status_wipes_only_on_revoked_for_this_serial(void) {
  TEST_ASSERT_TRUE(enrollStatusRevoked(200, "revoked", "c3a1", "c3a1"));
  TEST_ASSERT_FALSE(enrollStatusRevoked(200, "active", "c3a1", "c3a1"));
  TEST_ASSERT_FALSE(enrollStatusRevoked(200, "unknown", "c3a1", "c3a1"));
  TEST_ASSERT_FALSE(enrollStatusRevoked(200, "revoked", "ffff", "c3a1"));
  TEST_ASSERT_FALSE(enrollStatusRevoked(200, "revoked", "", "c3a1"));
  TEST_ASSERT_FALSE(enrollStatusRevoked(200, "", "c3a1", "c3a1"));
  TEST_ASSERT_FALSE(enrollStatusRevoked(403, "revoked", "c3a1", "c3a1"));
  TEST_ASSERT_FALSE(enrollStatusRevoked(500, "revoked", "c3a1", "c3a1"));
  TEST_ASSERT_FALSE(enrollStatusRevoked(-1, "revoked", "c3a1", "c3a1"));
  TEST_ASSERT_FALSE(enrollStatusRevoked(200, nullptr, "c3a1", "c3a1"));
  TEST_ASSERT_FALSE(enrollStatusRevoked(200, "revoked", "", ""));
}

void test_status_cadence_is_six_hours_plus_an_eighth(void) {
  TEST_ASSERT_EQUAL_UINT32(ENROLL_STATUS_EVERY_MS, enrollStatusCadenceMs(0));
  uint32_t top = enrollStatusCadenceMs(0xffffffffu);
  TEST_ASSERT_TRUE(top >= ENROLL_STATUS_EVERY_MS);
  TEST_ASSERT_TRUE(top <= ENROLL_STATUS_EVERY_MS + ENROLL_STATUS_EVERY_MS / 8);
}

// Unanswered checks back off from the enrollment floor, so even a unit that
// never gets an answer stays under the server's 30 requests/h per device.
void test_status_backoff_stays_under_the_server_rate_cap(void) {
  TEST_ASSERT_TRUE(enrollBackoffMs(0, 0) >= 3600000u / 30);
}

void test_hex_len_counts_only_lowercase_hex(void) {
  TEST_ASSERT_EQUAL_INT32(4, (int32_t)enrollHexLen("00ff"));
  TEST_ASSERT_EQUAL_INT32(0, (int32_t)enrollHexLen(""));
  TEST_ASSERT_EQUAL_INT32(-1, (int32_t)enrollHexLen("00FF"));
  TEST_ASSERT_EQUAL_INT32(-1, (int32_t)enrollHexLen(nullptr));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_serial_canonical_matches_the_app_form);
  RUN_TEST(test_serial_canonical_refuses_junk_and_overflow);
  RUN_TEST(test_serial_valid_is_canonical_only);
  RUN_TEST(test_status_message_is_the_signed_statement);
  RUN_TEST(test_status_message_refuses_unset_clock_and_bad_fields);
  RUN_TEST(test_status_body_carries_every_field);
  RUN_TEST(test_status_body_refuses_off_shape_values);
  RUN_TEST(test_status_wipes_only_on_revoked_for_this_serial);
  RUN_TEST(test_status_cadence_is_six_hours_plus_an_eighth);
  RUN_TEST(test_status_backoff_stays_under_the_server_rate_cap);
  RUN_TEST(test_hex_len_counts_only_lowercase_hex);
  RUN_TEST(test_hex_accepts_exact_lowercase);
  RUN_TEST(test_hex_refuses_case_length_and_junk);
  RUN_TEST(test_claim_code_is_eight_digits);
  RUN_TEST(test_claim_code_refuses_other_shapes);
  RUN_TEST(test_url_accepts_https_hosts);
  RUN_TEST(test_url_refuses_non_https);
  RUN_TEST(test_url_refuses_missing_host);
  RUN_TEST(test_url_refuses_query_fragment_userinfo_and_controls);
  RUN_TEST(test_url_refuses_over_cap);
  RUN_TEST(test_endpoint_joins_path_and_suffix);
  RUN_TEST(test_claim_link_drops_trailing_slashes);
  RUN_TEST(test_endpoint_drops_trailing_slashes);
  RUN_TEST(test_endpoint_refuses_bad_input_and_truncation);
  RUN_TEST(test_body_with_claim_token);
  RUN_TEST(test_body_with_signature);
  RUN_TEST(test_body_refuses_off_shape_values);
  RUN_TEST(test_body_refuses_truncation);
  RUN_TEST(test_backoff_starts_above_server_rate_cap);
  RUN_TEST(test_backoff_doubles_then_caps);
  RUN_TEST(test_backoff_jitter_stays_within_a_quarter);
  RUN_TEST(test_boot_announces_until_a_cert_is_stored);
  RUN_TEST(test_announce_moves_to_verify_only_on_200);
  RUN_TEST(test_verify_failure_reannounces);
  RUN_TEST(test_poll_waits_on_204_and_stores_on_200);
  RUN_TEST(test_ack_seals_only_on_200);
  RUN_TEST(test_non_http_steps_hold);
  RUN_TEST(test_host_accepts_dns_names);
  RUN_TEST(test_host_refuses_junk_and_over_cap);
  RUN_TEST(test_prefix_accepts_tenant_paths);
  RUN_TEST(test_prefix_refuses_wildcards_edges_and_over_cap);
  RUN_TEST(test_reply_needs_our_id_and_usable_fields);
  return UNITY_END();
}
