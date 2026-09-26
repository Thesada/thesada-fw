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

int main(int, char**) {
  UNITY_BEGIN();
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
