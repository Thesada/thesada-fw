#include <unity.h>
#include "config_scalar_policy.h"

static bool b; static long long i; static float f;

void setUp() {}
void tearDown() {}

void test_bool() {
  TEST_ASSERT_EQUAL(CFG_SCALAR_BOOL, configScalarKind("true", &b, &i, &f));
  TEST_ASSERT_TRUE(b);
  TEST_ASSERT_EQUAL(CFG_SCALAR_BOOL, configScalarKind("false", &b, &i, &f));
  TEST_ASSERT_FALSE(b);
}

void test_int_past_32_bits_keeps_its_value() {
  TEST_ASSERT_EQUAL(CFG_SCALAR_INT, configScalarKind("-1001234567890", &b, &i, &f));
  TEST_ASSERT_TRUE(i == -1001234567890LL);
  TEST_ASSERT_EQUAL(CFG_SCALAR_INT, configScalarKind("2147483648", &b, &i, &f));
  TEST_ASSERT_TRUE(i == 2147483648LL);
}

void test_int_past_64_bits_stays_string() {
  TEST_ASSERT_EQUAL(CFG_SCALAR_STRING, configScalarKind("99999999999999999999", &b, &i, &f));
  TEST_ASSERT_EQUAL(CFG_SCALAR_STRING, configScalarKind("-99999999999999999999", &b, &i, &f));
}

void test_float_and_string() {
  TEST_ASSERT_EQUAL(CFG_SCALAR_FLOAT, configScalarKind("1.5", &b, &i, &f));
  TEST_ASSERT_EQUAL_FLOAT(1.5f, f);
  TEST_ASSERT_EQUAL(CFG_SCALAR_STRING, configScalarKind("1e99", &b, &i, &f));
  TEST_ASSERT_EQUAL(CFG_SCALAR_STRING, configScalarKind("abc", &b, &i, &f));
  TEST_ASSERT_EQUAL(CFG_SCALAR_STRING, configScalarKind("", &b, &i, &f));
  TEST_ASSERT_EQUAL(CFG_SCALAR_STRING, configScalarKind("12ab", &b, &i, &f));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_bool);
  RUN_TEST(test_int_past_32_bits_keeps_its_value);
  RUN_TEST(test_int_past_64_bits_stays_string);
  RUN_TEST(test_float_and_string);
  return UNITY_END();
}
