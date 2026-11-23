// thesada-fw - cli_page_policy.h
// Pure cli_response page sizing: cut output to what a page holds, never clip.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>

// PUBLISH overhead in PubSubClient's buffer: 5-byte header, 2-byte topic length.
#define CLI_MQTT_FIXED_HEADER 7

// Bytes a split piece adds to its page: ,"cont":true
#define CLI_CONT_COST 12

// Most bytes one more output string adds beside its escaped text: quotes, comma.
#define CLI_STR_OVERHEAD 3

// Headroom between page limit and reply cap for more, a last ok:false and NUL.
#define CLI_PAGE_MARGIN 160

// PubSubClient sends from its one buffer (mqtt.buffer_in), so a reply must fit
// it beside header and topic, not only buffer_out. out: payload byte cap.
inline size_t cliReplyCap(size_t bufOut, size_t bufIn, size_t topicLen) {
  size_t fixed = CLI_MQTT_FIXED_HEADER + topicLen;
  size_t in = bufIn > fixed ? bufIn - fixed : 0;
  return bufOut < in ? bufOut : in;
}

// Bytes a page may measure with its strings and cont, before more is added.
inline size_t cliPageLimit(size_t replyCap) {
  return replyCap > CLI_PAGE_MARGIN ? replyCap - CLI_PAGE_MARGIN : replyCap / 2;
}

// Bytes one char takes inside a JSON string as ArduinoJson 7.4 TextFormatter
// writes it: '"', '\\' and \b \f \n \r \t take two, NUL six.
inline size_t cliJsonCharCost(unsigned char c) {
  switch (c) {
    case '"': case '\\': case '\b': case '\f': case '\n': case '\r': case '\t':
      return 2;
    case '\0':
      return 6;
    default:
      return 1;
  }
}

// Escaped size of s[0..n), quotes excluded.
inline size_t cliJsonStrLen(const char* s, size_t n) {
  if (!s) return 0;
  size_t total = 0;
  for (size_t i = 0; i < n; i++) total += cliJsonCharCost((unsigned char)s[i]);
  return total;
}

// Longest m <= n where s[0..m) does not end inside a UTF-8 sequence. A piece
// cut mid-character decodes to U+FFFD at the consumer.
inline size_t cliUtf8Cut(const char* s, size_t n) {
  if (!s) return 0;
  for (size_t back = 0; back < 4 && back < n; back++) {
    unsigned char c = (unsigned char)s[n - 1 - back];
    if ((c & 0xC0) == 0x80) continue;
    size_t seq = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
               : (c & 0xF8) == 0xF0 ? 4 : 1;
    return seq > back + 1 ? n - 1 - back : n;
  }
  return n;
}

// Longest piece of s[0..n) whose escaped size fits budget, on a UTF-8 boundary
// unless it is all of s. out: piece length in bytes, 0 if nothing fits.
inline size_t cliJsonFit(const char* s, size_t n, size_t budget) {
  if (!s) return 0;
  size_t used = 0;
  size_t m = 0;
  while (m < n) {
    size_t c = cliJsonCharCost((unsigned char)s[m]);
    if (used + c > budget) break;
    used += c;
    m++;
  }
  return m == n ? n : cliUtf8Cut(s, m);
}

// Bytes of s[0..n) for a page measuring pageUsed, *split true when not all of it
// fits: flush a page with output, else ship the piece with cont. 0 split: none fits.
inline size_t cliNextPiece(const char* s, size_t n, size_t pageUsed,
                           size_t pageLimit, bool* split) {
  if (!s) n = 0;
  size_t used = pageUsed + CLI_STR_OVERHEAD;
  size_t room = used < pageLimit ? pageLimit - used : 0;
  bool whole = used <= pageLimit && cliJsonStrLen(s, n) <= room;
  if (split) *split = !whole;
  if (whole) return n;
  return room > CLI_CONT_COST ? cliJsonFit(s, n, room - CLI_CONT_COST) : 0;
}
