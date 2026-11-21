// Host-native unit tests for cell_at_policy.h (console nesting on the AT bus,
// modem line reads and the URCs a raw read hands back to the sockets).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <string.h>
#include "cell_at_policy.h"

void setUp(void) {}
void tearDown(void) {}

// --- console nested inside a held exchange ----------------------------------------

// #112 M7: a cell.at typed during the +CNTP: wait re-took the recursive mutex
// on the same task and ran in the middle of the exchange.
void test_console_inside_a_held_exchange_is_refused_the_bus(void) {
  int loopTask = 0;
  TEST_ASSERT_TRUE(cellAtNestedRefused(1, &loopTask, &loopTask));
  TEST_ASSERT_TRUE(cellAtNestedRefused(2, &loopTask, &loopTask));
}

// Outside a held pump the same task re-takes the guard, as every nested
// exchange in the driver does.
void test_recursion_outside_a_console_pump_is_allowed(void) {
  int loopTask = 0;
  TEST_ASSERT_FALSE(cellAtNestedRefused(0, &loopTask, &loopTask));
}

// pause() gives the bus back before it pumps, and another task waits as usual.
void test_free_bus_or_other_holder_is_not_refused(void) {
  int loopTask = 0, fetchTask = 0;
  TEST_ASSERT_FALSE(cellAtNestedRefused(1, nullptr, &loopTask));
  TEST_ASSERT_FALSE(cellAtNestedRefused(1, &loopTask, &fetchTask));
}

// --- line reader ---------------------------------------------------------------------

static bool feedAll(CellLine<16>& r, const char* s, int* lines) {
  bool last = false;
  for (; *s; ++s) {
    last = r.feed(*s);
    if (last) ++*lines;
  }
  return last;
}

void test_line_reader_strips_cr_and_skips_empty_lines(void) {
  CellLine<16> r;
  int lines = 0;
  TEST_ASSERT_TRUE(feedAll(r, "\r\n\r\n+CNTP: 1\r\n", &lines));
  TEST_ASSERT_EQUAL_INT(1, lines);
  TEST_ASSERT_EQUAL_STRING("+CNTP: 1", r.buf);
}

// A line longer than the buffer is dropped whole, never parsed from its tail.
void test_line_reader_drops_an_overlong_line_whole(void) {
  CellLine<16> r;
  int lines = 0;
  feedAll(r, "+CDNSGIP: 1,\"0123456789\",\"+CNTP: 1\"\n", &lines);
  TEST_ASSERT_EQUAL_INT(0, lines);
  TEST_ASSERT_TRUE(feedAll(r, "OK\n", &lines));
  TEST_ASSERT_EQUAL_STRING("OK", r.buf);
}

void test_line_reader_takes_a_line_that_just_fits(void) {
  CellLine<16> r;
  int lines = 0;
  TEST_ASSERT_TRUE(feedAll(r, "123456789012345\n", &lines));
  TEST_ASSERT_EQUAL_STRING("123456789012345", r.buf);
  TEST_ASSERT_FALSE(feedAll(r, "1234567890123456\n", &lines));
  TEST_ASSERT_EQUAL_INT(1, lines);
}

// --- socket URCs -----------------------------------------------------------------------

// #112 L21: these arrived inside a raw read and never reached the socket.
void test_castate_not_connected_closes_its_slot(void) {
  CellUrc u = cellSocketUrc("+CASTATE: 1,0", 2);
  TEST_ASSERT_EQUAL_UINT8(CELL_URC_CLOSED, u.kind);
  TEST_ASSERT_EQUAL_UINT8(1, u.mux);
  u = cellSocketUrc("+CASTATE: 0,2", 2);
  TEST_ASSERT_EQUAL_UINT8(CELL_URC_CLOSED, u.kind);
  TEST_ASSERT_EQUAL_UINT8(0, u.mux);
}

void test_castate_connected_changes_nothing(void) {
  TEST_ASSERT_EQUAL_UINT8(CELL_URC_NONE, cellSocketUrc("+CASTATE: 0,1", 2).kind);
}

void test_cadataind_marks_data_on_its_slot(void) {
  CellUrc u = cellSocketUrc("+CADATAIND: 1", 2);
  TEST_ASSERT_EQUAL_UINT8(CELL_URC_DATA, u.kind);
  TEST_ASSERT_EQUAL_UINT8(1, u.mux);
}

void test_urc_for_a_slot_out_of_range_or_malformed_is_ignored(void) {
  TEST_ASSERT_EQUAL_UINT8(CELL_URC_NONE, cellSocketUrc("+CADATAIND: 2", 2).kind);
  TEST_ASSERT_EQUAL_UINT8(CELL_URC_NONE, cellSocketUrc("+CASTATE: 5,0", 2).kind);
  TEST_ASSERT_EQUAL_UINT8(CELL_URC_NONE, cellSocketUrc("+CASTATE: -1,0", 2).kind);
  TEST_ASSERT_EQUAL_UINT8(CELL_URC_NONE, cellSocketUrc("+CASTATE: 0", 2).kind);
  TEST_ASSERT_EQUAL_UINT8(CELL_URC_NONE, cellSocketUrc("+CASTATE: x,0", 2).kind);
  TEST_ASSERT_EQUAL_UINT8(CELL_URC_NONE, cellSocketUrc("+CADATAIND:", 2).kind);
  TEST_ASSERT_EQUAL_UINT8(CELL_URC_NONE, cellSocketUrc("+CADATAIND: 1x", 2).kind);
  TEST_ASSERT_EQUAL_UINT8(CELL_URC_NONE, cellSocketUrc("OK", 2).kind);
  TEST_ASSERT_EQUAL_UINT8(CELL_URC_NONE, cellSocketUrc(nullptr, 2).kind);
}

// --- +CNTP: and +CDNSGIP: ---------------------------------------------------------------

void test_ntp_code_reads_the_result(void) {
  TEST_ASSERT_EQUAL_INT(1, cellNtpCode("+CNTP: 1"));
  TEST_ASSERT_EQUAL_INT(61, cellNtpCode("+CNTP: 61"));
  TEST_ASSERT_EQUAL_INT(0, cellNtpCode("+CNTP: x"));
  TEST_ASSERT_EQUAL_INT(-1, cellNtpCode("+CASTATE: 0,0"));
  TEST_ASSERT_EQUAL_INT(-1, cellNtpCode(nullptr));
}

void test_dnsgip_takes_the_first_address_on_success(void) {
  char ip[48];
  TEST_ASSERT_TRUE(cellDnsgipIp("+CDNSGIP: 1,\"a.example\",\"192.0.2.7\",\"192.0.2.8\"", ip, sizeof(ip)));
  TEST_ASSERT_EQUAL_STRING("192.0.2.7", ip);
}

void test_dnsgip_failure_or_malformed_resolves_nothing(void) {
  char ip[48] = "keep";
  TEST_ASSERT_FALSE(cellDnsgipIp("+CDNSGIP: 0,8", ip, sizeof(ip)));
  TEST_ASSERT_FALSE(cellDnsgipIp("+CDNSGIP: 1,\"a.example\"", ip, sizeof(ip)));
  TEST_ASSERT_FALSE(cellDnsgipIp("+CDNSGIP: 1,\"a.example\",\"\"", ip, sizeof(ip)));
  TEST_ASSERT_FALSE(cellDnsgipIp("+CDNSGIP: 1,\"a.example\",\"192.0.2.7", ip, sizeof(ip)));
  TEST_ASSERT_FALSE(cellDnsgipIp("+CDNSGIP: 1", ip, sizeof(ip)));
  TEST_ASSERT_FALSE(cellDnsgipIp("+CNTP: 1", ip, sizeof(ip)));
  TEST_ASSERT_FALSE(cellDnsgipIp(nullptr, ip, sizeof(ip)));
  TEST_ASSERT_EQUAL_STRING("keep", ip);
}

// An address that does not fit is refused, never clipped.
void test_dnsgip_address_that_does_not_fit_is_refused(void) {
  char ip[8] = "keep";
  TEST_ASSERT_FALSE(cellDnsgipIp("+CDNSGIP: 1,\"a\",\"192.0.2.77\"", ip, sizeof(ip)));
  TEST_ASSERT_EQUAL_STRING("keep", ip);
  TEST_ASSERT_TRUE(cellDnsgipIp("+CDNSGIP: 1,\"a\",\"1.2.3.4\"", ip, sizeof(ip)));
  TEST_ASSERT_EQUAL_STRING("1.2.3.4", ip);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_console_inside_a_held_exchange_is_refused_the_bus);
  RUN_TEST(test_recursion_outside_a_console_pump_is_allowed);
  RUN_TEST(test_free_bus_or_other_holder_is_not_refused);
  RUN_TEST(test_line_reader_strips_cr_and_skips_empty_lines);
  RUN_TEST(test_line_reader_drops_an_overlong_line_whole);
  RUN_TEST(test_line_reader_takes_a_line_that_just_fits);
  RUN_TEST(test_castate_not_connected_closes_its_slot);
  RUN_TEST(test_castate_connected_changes_nothing);
  RUN_TEST(test_cadataind_marks_data_on_its_slot);
  RUN_TEST(test_urc_for_a_slot_out_of_range_or_malformed_is_ignored);
  RUN_TEST(test_ntp_code_reads_the_result);
  RUN_TEST(test_dnsgip_takes_the_first_address_on_success);
  RUN_TEST(test_dnsgip_failure_or_malformed_resolves_nothing);
  RUN_TEST(test_dnsgip_address_that_does_not_fit_is_refused);
  return UNITY_END();
}
