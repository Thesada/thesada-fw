// Host-native unit tests for http_url_policy.h (URL split, status line).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "http_url_policy.h"

void setUp(void) {}
void tearDown(void) {}

void test_split_https_default_port(void) {
  char host[32]; const char* path; uint16_t port; bool https = false;
  TEST_ASSERT_TRUE(httpUrlSplit("https://api.telegram.org/bot1/sendMessage", host, sizeof(host), &path, &port, &https));
  TEST_ASSERT_EQUAL_STRING("api.telegram.org", host);
  TEST_ASSERT_EQUAL_STRING("/bot1/sendMessage", path);
  TEST_ASSERT_EQUAL_UINT16(443, port);
  TEST_ASSERT_TRUE(https);
}

void test_split_http_port_and_bare_host(void) {
  char host[32]; const char* path; uint16_t port; bool https = true;
  TEST_ASSERT_TRUE(httpUrlSplit("http://10.0.0.5:8123", host, sizeof(host), &path, &port, &https));
  TEST_ASSERT_EQUAL_STRING("10.0.0.5", host);
  TEST_ASSERT_EQUAL_STRING("/", path);
  TEST_ASSERT_EQUAL_UINT16(8123, port);
  TEST_ASSERT_FALSE(https);
}

void test_split_refuses_junk(void) {
  char host[8]; const char* path; uint16_t port;
  TEST_ASSERT_FALSE(httpUrlSplit("ftp://x/y", host, sizeof(host), &path, &port));
  TEST_ASSERT_FALSE(httpUrlSplit("https:///path", host, sizeof(host), &path, &port));
  TEST_ASSERT_FALSE(httpUrlSplit("https://a-host-too-long.example/", host, sizeof(host), &path, &port));
  TEST_ASSERT_FALSE(httpUrlSplit("https://h:0/", host, sizeof(host), &path, &port));
  TEST_ASSERT_FALSE(httpUrlSplit("https://h:80x/", host, sizeof(host), &path, &port));
  TEST_ASSERT_FALSE(httpUrlSplit("https://h:70000/", host, sizeof(host), &path, &port));
  TEST_ASSERT_FALSE(httpUrlSplit("https://h:/", host, sizeof(host), &path, &port));
  TEST_ASSERT_FALSE(httpUrlSplit(nullptr, host, sizeof(host), &path, &port));
  // Negative control: the same shape with a short host passes.
  TEST_ASSERT_TRUE(httpUrlSplit("https://h/", host, sizeof(host), &path, &port));
}

void test_status_code(void) {
  TEST_ASSERT_EQUAL_INT(200, httpStatusCode("HTTP/1.1 200 OK"));
  TEST_ASSERT_EQUAL_INT(429, httpStatusCode("HTTP/1.0 429 Too Many Requests"));
  TEST_ASSERT_EQUAL_INT(-1, httpStatusCode("HTTP/1.1"));
  TEST_ASSERT_EQUAL_INT(-1, httpStatusCode("SSH-2.0-OpenSSH"));
  TEST_ASSERT_EQUAL_INT(-1, httpStatusCode("HTTP/1.1 999 nope"));
  TEST_ASSERT_EQUAL_INT(-1, httpStatusCode("HTTP/1.1 20 short"));
  TEST_ASSERT_EQUAL_INT(-1, httpStatusCode(nullptr));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_split_https_default_port);
  RUN_TEST(test_split_http_port_and_bare_host);
  RUN_TEST(test_split_refuses_junk);
  RUN_TEST(test_status_code);
  return UNITY_END();
}
