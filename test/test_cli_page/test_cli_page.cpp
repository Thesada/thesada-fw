// Host-native unit tests for cli_page_policy.h (cli_response page sizing).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <string.h>
#include <string>
#include <vector>
#include "cli_page_policy.h"

void setUp(void) {}
void tearDown(void) {}

// Is s[0..n) free of a multi-byte sequence cut short at its end?
static bool endsOnUtf8Boundary(const char* s, size_t n) {
  return cliUtf8Cut(s, n) == n;
}

void test_char_cost_matches_arduinojson_escapes(void) {
  const char two[] = {'"', '\\', '\b', '\f', '\n', '\r', '\t'};
  for (char c : two) TEST_ASSERT_EQUAL_UINT(2, (unsigned)cliJsonCharCost((unsigned char)c));
  TEST_ASSERT_EQUAL_UINT(6, (unsigned)cliJsonCharCost(0));
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)cliJsonCharCost('a'));
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)cliJsonCharCost('/'));
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)cliJsonCharCost(0x01));
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)cliJsonCharCost(0xC3));
}

void test_escaped_length(void) {
  const char* s = "a\"b\\c\n";
  TEST_ASSERT_EQUAL_UINT(9, (unsigned)cliJsonStrLen(s, strlen(s)));
  const char nul[] = {'x', '\0', 'y'};
  TEST_ASSERT_EQUAL_UINT(8, (unsigned)cliJsonStrLen(nul, sizeof(nul)));
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)cliJsonStrLen(nullptr, 4));
}

void test_utf8_cut(void) {
  TEST_ASSERT_EQUAL_UINT(3, (unsigned)cliUtf8Cut("abc", 3));
  TEST_ASSERT_EQUAL_UINT(2, (unsigned)cliUtf8Cut("ab\xC3\xA9", 3));       // e-acute cut after its lead
  TEST_ASSERT_EQUAL_UINT(4, (unsigned)cliUtf8Cut("ab\xC3\xA9", 4));       // whole
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)cliUtf8Cut("\xE2\x82\xAC", 2));     // euro sign, 2 of 3
  TEST_ASSERT_EQUAL_UINT(3, (unsigned)cliUtf8Cut("\xE2\x82\xAC", 3));
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)cliUtf8Cut("a\xF0\x9F\x98\x80", 4));  // emoji, 3 of 4
  TEST_ASSERT_EQUAL_UINT(5, (unsigned)cliUtf8Cut("a\xF0\x9F\x98\x80", 5));
  TEST_ASSERT_EQUAL_UINT(4, (unsigned)cliUtf8Cut("\x80\x80\x80\x80", 4));  // no lead: not ours to fix
  TEST_ASSERT_EQUAL_UINT(2, (unsigned)cliUtf8Cut("a\xFF", 2));            // invalid lead is one byte
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)cliUtf8Cut("", 0));
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)cliUtf8Cut(nullptr, 3));
}

void test_fit_whole_string(void) {
  TEST_ASSERT_EQUAL_UINT(5, (unsigned)cliJsonFit("hello", 5, 5));
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)cliJsonFit("", 0, 0));
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)cliJsonFit(nullptr, 5, 100));
}

// An escape is never split: one byte of room cannot hold a two-byte quote.
void test_fit_never_splits_an_escape(void) {
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)cliJsonFit("\"x", 2, 1));
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)cliJsonFit("\"x", 2, 2));
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)cliJsonFit("a\"", 2, 2));
}

void test_fit_backs_off_to_utf8_boundary(void) {
  // "ab" + e-acute: three bytes of budget fit "ab" and the lead only.
  TEST_ASSERT_EQUAL_UINT(2, (unsigned)cliJsonFit("ab\xC3\xA9", 4, 3));
  TEST_ASSERT_EQUAL_UINT(4, (unsigned)cliJsonFit("ab\xC3\xA9", 4, 4));
}

// Split s with cliJsonFit at one budget and check every piece.
static void assertSplits(const std::string& line, size_t budget, size_t minPieces) {
  std::string joined;
  size_t pieces = 0;
  const char* p = line.data();
  size_t left = line.size();
  while (left > 0) {
    size_t n = cliJsonFit(p, left, budget);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "no progress");
    TEST_ASSERT_TRUE(cliJsonStrLen(p, n) <= budget);
    TEST_ASSERT_TRUE(endsOnUtf8Boundary(p, n));
    joined.append(p, n);
    p += n;
    left -= n;
    pieces++;
  }
  TEST_ASSERT_TRUE(pieces >= minPieces);
  TEST_ASSERT_TRUE(joined == line);
}

// Quote s as ArduinoJson 7.4 TextFormatter writes it.
static std::string jsonQuote(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\b': o += "\\b"; break;
      case '\f': o += "\\f"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      case '\0': o += "\\u0000"; break;
      default:   o += (char)c;
    }
  }
  return o + "\"";
}

// runCli's sink over pages serialized in resp's key order. Each page is checked
// as it ships: strings and cont within the page limit, the whole reply under cap.
struct PageSim {
  size_t replyCap;
  size_t pageLimit;
  std::string head;
  int pageNum = 0;
  std::vector<std::string> out;
  bool cont = false;
  bool dropped = false;
  std::vector<std::vector<std::string>> pages;
  std::vector<bool> conts;

  PageSim(size_t cap, const std::string& reqId)
      : replyCap(cap), pageLimit(cliPageLimit(cap)),
        head("{\"cmd\":\"config.dump\"" +
             (reqId.empty() ? std::string() : ",\"req_id\":" + jsonQuote(reqId))) {}

  std::string json(bool ok, int more) const {
    std::string j = head + ",\"ok\":" + (ok ? "true" : "false") +
                    ",\"page\":" + std::to_string(pageNum) + ",\"output\":[";
    for (size_t i = 0; i < out.size(); i++) j += (i ? "," : "") + jsonQuote(out[i]);
    j += ']';
    if (cont) j += ",\"cont\":true";
    if (more >= 0) j += more ? ",\"more\":true" : ",\"more\":false";
    return j + "}";
  }

  void publish(bool more, bool ok) {
    TEST_ASSERT_TRUE_MESSAGE(json(true, -1).size() <= pageLimit, "page past its limit");
    TEST_ASSERT_TRUE_MESSAGE(json(ok, more).size() < replyCap, "reply past cap");
    pages.push_back(out);
    conts.push_back(cont);
    pageNum++;
    out.clear();
    cont = false;
  }

  void sink(const std::string& line) {
    const char* p = line.data();
    size_t len = line.size();
    for (;;) {
      bool split = false;
      size_t n = cliNextPiece(p, len, json(true, -1).size(), pageLimit, &split);
      if (split && !out.empty()) { publish(true, true); continue; }
      if (split && n == 0) { dropped = true; return; }
      TEST_ASSERT_TRUE(endsOnUtf8Boundary(p, n));
      out.emplace_back(p, n);
      if (!split) return;
      cont = true;
      publish(true, true);
      p += n;
      len -= n;
    }
  }

  // Lines back from the pages: a cont page's last string runs into the next.
  std::vector<std::string> joined() const {
    std::vector<std::string> lines;
    std::string acc;
    bool open = false;
    for (size_t pg = 0; pg < pages.size(); pg++) {
      for (size_t i = 0; i < pages[pg].size(); i++) {
        acc += pages[pg][i];
        open = conts[pg] && i + 1 == pages[pg].size();
        if (!open) { lines.push_back(acc); acc.clear(); }
      }
    }
    TEST_ASSERT_FALSE_MESSAGE(open, "last page ends on cont");
    return lines;
  }
};

// Run lines through the sink at cap and check they join back whole.
static size_t assertPagesJoin(const std::vector<std::string>& lines, size_t cap,
                              const std::string& reqId) {
  PageSim sim(cap, reqId);
  for (const auto& l : lines) sim.sink(l);
  sim.publish(false, false);
  TEST_ASSERT_FALSE(sim.dropped);
  TEST_ASSERT_TRUE(sim.joined() == lines);
  return sim.pages.size();
}

// thesada-fw#53: config.dump prints the whole config as one line. Past a page it
// was serialized into the fixed buffer and clipped. It must go out in pieces
// whose pages each fit the reply cap and join back to the line.
void test_oversized_line_splits_into_fitting_pages(void) {
  std::string cfg = "{";
  while (cfg.size() < 6000) cfg += "\"mqtt.key\":\"va\\\"lue\",";
  cfg += "\"end\":1}";
  size_t big = cliReplyCap(4096, 4096, 159);
  size_t cyd = cliReplyCap(1024, 1024, 159);
  TEST_ASSERT_TRUE(cliJsonStrLen(cfg.data(), cfg.size()) > cliPageLimit(big));
  TEST_ASSERT_TRUE(assertPagesJoin({"config:", cfg, ""}, big, "req-42") >= 3);
  TEST_ASSERT_TRUE(assertPagesJoin({cfg}, cyd, "") >= 9);
}

// Every line length around a page boundary, so pieces land on the limit itself.
void test_pages_fill_to_the_limit(void) {
  const size_t caps[] = {300, cliReplyCap(1024, 1024, 159)};
  for (size_t cap : caps) {
    size_t lim = cliPageLimit(cap);
    for (size_t len = 1; len < 2 * lim + 40; len++) {
      std::string a(len, 'a');
      std::string q(len / 2, '"');
      assertPagesJoin({a, "x", a, q, a}, cap, "r");
    }
  }
}

// A line that cannot start on an empty page is dropped, never sent past cap.
void test_line_with_no_room_is_dropped(void) {
  PageSim sim(300, std::string(cliPageLimit(300), 'r'));
  sim.sink("hello");
  TEST_ASSERT_TRUE(sim.dropped);
  TEST_ASSERT_TRUE(sim.out.empty());
}

void test_next_piece_edges(void) {
  bool split = true;
  TEST_ASSERT_EQUAL_UINT(7, (unsigned)cliNextPiece("abcdefg", 7, 10, 20, &split));
  TEST_ASSERT_FALSE(split);
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)cliNextPiece("abcdefgh", 8, 10, 20, &split));
  TEST_ASSERT_TRUE(split);
  // Split pieces leave room for cont: 27 bytes free, 15 of text.
  std::string t(30, 't');
  TEST_ASSERT_EQUAL_UINT(15, (unsigned)cliNextPiece(t.data(), t.size(), 10, 40, &split));
  TEST_ASSERT_TRUE(split);
  // An empty line takes its quotes too.
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)cliNextPiece("", 0, 37, 40, &split));
  TEST_ASSERT_FALSE(split);
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)cliNextPiece("", 0, 38, 40, &split));
  TEST_ASSERT_TRUE(split);
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)cliNextPiece("abc", 3, 50, 40, &split));
  TEST_ASSERT_TRUE(split);
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)cliNextPiece(nullptr, 5, 10, 40, &split));
  TEST_ASSERT_FALSE(split);
  TEST_ASSERT_EQUAL_UINT(3, (unsigned)cliNextPiece("abc", 3, 0, 40, nullptr));
}

void test_page_limit(void) {
  TEST_ASSERT_EQUAL_UINT(3930 - 160, (unsigned)cliPageLimit(3930));
  TEST_ASSERT_EQUAL_UINT(1, (unsigned)cliPageLimit(161));
  TEST_ASSERT_EQUAL_UINT(80, (unsigned)cliPageLimit(160));
  TEST_ASSERT_EQUAL_UINT(50, (unsigned)cliPageLimit(100));
}

// Multi-byte text across every cut stays whole on both sides.
void test_split_keeps_utf8_characters_whole(void) {
  std::string s;
  while (s.size() < 3000) s += "temp \xC2\xB0" "C \xE2\x82\xAC \xF0\x9F\x94\xA5 ";
  for (size_t budget = 7; budget < 40; budget++) assertSplits(s, budget, 2);
  TEST_ASSERT_TRUE(assertPagesJoin({s, s}, 300, "r") >= 2);
}

// fs.cat: 2048 quotes escape to 4096 bytes, past a 4096 buffer with envelope.
void test_quote_heavy_chunk_is_trimmed_to_room(void) {
  std::string q(2048, '"');
  size_t room = 4096 - 120;
  size_t n = cliJsonFit(q.data(), q.size(), room);
  TEST_ASSERT_EQUAL_UINT(room / 2, (unsigned)n);
  TEST_ASSERT_TRUE(cliJsonStrLen(q.data(), n) <= room);
}

void test_reply_cap(void) {
  TEST_ASSERT_EQUAL_UINT(4096 - 7 - 159, (unsigned)cliReplyCap(4096, 4096, 159));
  // buffer_out past buffer_in: PubSubClient's buffer is the limit.
  TEST_ASSERT_EQUAL_UINT(4096 - 7 - 159, (unsigned)cliReplyCap(8192, 4096, 159));
  TEST_ASSERT_EQUAL_UINT(2048, (unsigned)cliReplyCap(2048, 8192, 159));
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)cliReplyCap(4096, 100, 159));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_char_cost_matches_arduinojson_escapes);
  RUN_TEST(test_escaped_length);
  RUN_TEST(test_utf8_cut);
  RUN_TEST(test_fit_whole_string);
  RUN_TEST(test_fit_never_splits_an_escape);
  RUN_TEST(test_fit_backs_off_to_utf8_boundary);
  RUN_TEST(test_oversized_line_splits_into_fitting_pages);
  RUN_TEST(test_pages_fill_to_the_limit);
  RUN_TEST(test_line_with_no_room_is_dropped);
  RUN_TEST(test_next_piece_edges);
  RUN_TEST(test_page_limit);
  RUN_TEST(test_split_keeps_utf8_characters_whole);
  RUN_TEST(test_quote_heavy_chunk_is_trimmed_to_room);
  RUN_TEST(test_reply_cap);
  return UNITY_END();
}
