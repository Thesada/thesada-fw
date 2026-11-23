// Host-native unit tests for console_drop_policy.h (console never waits on a stalled USB host).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <string>
#include "console_drop_policy.h"

// HWCDC model (arduino-esp32 3.3.12): a 256 B TX ring. A send that does not
// fit retries 20 x tx_timeout_ms (50 ms) while the host is enumerated but not
// reading, then returns 0. A reading host drains 64 B per ms.
struct FakeCdc {
  size_t cap = 256;
  size_t queued = 0;
  bool reading = false;
  bool refuse = false;
  uint32_t clock = 0;
  std::string out;

  int availableForWrite() { return (int)(cap - queued); }
  size_t write(const uint8_t* b, size_t n) {
    while (n > cap - queued) {
      if (!reading) { clock += 20 * 50; return 0; }
      sleepMs(1);
    }
    if (refuse) return 0;
    out.append(reinterpret_cast<const char*>(b), n);
    queued += n;
    return n;
  }
  uint32_t nowMs() { return clock; }
  void sleepMs(uint32_t ms) {
    clock += ms;
    if (reading) queued = queued > 64 * ms ? queued - 64 * ms : 0;
  }
  void drain() { queued = 0; }
};

static std::string line(size_t len, char c) { return std::string(len, c); }

void setUp(void) {}
void tearDown(void) {}

// The finding: host attached, nobody reading, ring full. 15 boot lines.
void test_stalled_host_costs_bounded_time(void) {
  FakeCdc p; p.queued = p.cap;
  ConsoleDropState st;
  for (int i = 0; i < 15; i++) {
    TEST_ASSERT_FALSE(consoleWriteLine(p, st, "[INF][PowerManager] power.led_mode mode=off"));
  }
  TEST_ASSERT_TRUE(p.clock <= CONSOLE_GRACE_MS);
  TEST_ASSERT_EQUAL_UINT32(15, st.dropped);
  TEST_ASSERT_EQUAL_size_t(0, p.out.size());
}

// Room for one line: it goes out whole, the next is dropped whole.
void test_drops_whole_lines_only(void) {
  FakeCdc p; p.queued = p.cap - 100;
  ConsoleDropState st;
  const std::string a = line(60, 'a');
  TEST_ASSERT_TRUE(consoleWriteLine(p, st, a.c_str()));
  TEST_ASSERT_FALSE(consoleWriteLine(p, st, a.c_str()));
  TEST_ASSERT_EQUAL_STRING((a + "\r\n").c_str(), p.out.c_str());
  TEST_ASSERT_EQUAL_UINT32(1, st.dropped);
}

// Once the host reads again, one notice carries the count, then the line.
void test_dropped_count_reported_once(void) {
  FakeCdc p; p.queued = p.cap;
  ConsoleDropState st;
  for (int i = 0; i < 5; i++) consoleWriteLine(p, st, "x");
  p.drain();
  TEST_ASSERT_TRUE(consoleWriteLine(p, st, "back"));
  TEST_ASSERT_TRUE(consoleWriteLine(p, st, "again"));
  TEST_ASSERT_EQUAL_STRING("[WRN][Console] console.dropped n=5\r\nback\r\nagain\r\n", p.out.c_str());
  TEST_ASSERT_EQUAL_UINT32(0, st.dropped);
}

// Still stalled after drops: no grace wait again, count keeps climbing.
void test_stall_waits_grace_once(void) {
  FakeCdc p; p.queued = p.cap;
  ConsoleDropState st;
  consoleWriteLine(p, st, "a");
  const uint32_t first = p.clock;
  consoleWriteLine(p, st, "b");
  TEST_ASSERT_EQUAL_UINT32(first, p.clock);
  TEST_ASSERT_EQUAL_UINT32(2, st.dropped);
}

// Room only for the notice: it goes out, the line is the first of a new count.
void test_notice_then_line_too_big(void) {
  FakeCdc p; p.queued = p.cap;
  ConsoleDropState st;
  consoleWriteLine(p, st, "a");
  p.queued = p.cap - 50;
  const std::string big = line(100, 'b');
  TEST_ASSERT_FALSE(consoleWriteLine(p, st, big.c_str()));
  TEST_ASSERT_EQUAL_STRING("[WRN][Console] console.dropped n=1\r\n", p.out.c_str());
  TEST_ASSERT_EQUAL_UINT32(1, st.dropped);
}

// A host that reads: nothing dropped, bytes identical to println.
void test_reading_host_byte_identical(void) {
  FakeCdc p; p.reading = true;
  ConsoleDropState st;
  std::string want;
  for (int i = 0; i < 20; i++) {
    const std::string l = line(200, (char)('a' + i));
    TEST_ASSERT_TRUE(consoleWriteLine(p, st, l.c_str()));
    want += l + "\r\n";
  }
  TEST_ASSERT_EQUAL_UINT32(0, st.dropped);
  TEST_ASSERT_TRUE(want == p.out);
}

// A line past the frame is cut to it and still ends in CRLF.
void test_long_line_fits_frame(void) {
  FakeCdc p;
  ConsoleDropState st;
  const std::string l = line(400, 'z');
  TEST_ASSERT_TRUE(consoleWriteLine(p, st, l.c_str()));
  TEST_ASSERT_EQUAL_size_t(CONSOLE_FRAME_MAX, p.out.size());
  TEST_ASSERT_EQUAL_STRING("\r\n", p.out.substr(CONSOLE_FRAME_MAX - 2).c_str());
}

// A short write is a drop, never a success.
void test_refused_write_counts_as_drop(void) {
  FakeCdc p; p.refuse = true;
  ConsoleDropState st;
  TEST_ASSERT_FALSE(consoleWriteLine(p, st, "a"));
  TEST_ASSERT_EQUAL_UINT32(1, st.dropped);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_stalled_host_costs_bounded_time);
  RUN_TEST(test_drops_whole_lines_only);
  RUN_TEST(test_dropped_count_reported_once);
  RUN_TEST(test_stall_waits_grace_once);
  RUN_TEST(test_notice_then_line_too_big);
  RUN_TEST(test_reading_host_byte_identical);
  RUN_TEST(test_long_line_fits_frame);
  RUN_TEST(test_refused_write_counts_as_drop);
  return UNITY_END();
}
