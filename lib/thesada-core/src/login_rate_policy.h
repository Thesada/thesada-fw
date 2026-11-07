// thesada-fw - login_rate_policy.h
// Login lockout table: LOGIN_RATE_MAX_FAILS counted guesses from one source IP
// lock it out for LOGIN_RATE_LOCKOUT_MS; after it one more wrong guess re-locks.
// An IP is forgotten on success or after LOGIN_RATE_QUIET_MS with no failure.
// A full table evicts nothing: every IP without a slot shares one overflow entry.
// No Arduino deps, host-unit-testable.
// Time is 64-bit uptime ms, so no deadline goes stale at the millis() wrap.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

constexpr uint8_t  LOGIN_RATE_MAX_FAILS  = 5;
constexpr uint64_t LOGIN_RATE_LOCKOUT_MS = 30000;
constexpr uint64_t LOGIN_RATE_QUIET_MS   = 600000;
constexpr size_t   LOGIN_RATE_IP_CAP     = 46;  // INET6_ADDRSTRLEN

template <size_t N = 16>
class LoginRateTable {
 public:
  // False while ip is locked out, or while every slot holds another live ip
  // and the overflow entry is locked. in: source ip, uptime ms. out: true to serve.
  bool allow(const char* ip, uint64_t now) {
    const Entry* s = find(ip, now);
    if (!s && ip && *ip && freeSlot(now) == N) s = &overflow(now);
    return !s || s->fails < LOGIN_RATE_MAX_FAILS;
  }

  // Count one wrong guess; the limit-th starts the lockout. A new ip takes a
  // free or quiet slot, else it counts on the overflow entry.
  // in: source ip, uptime ms.
  void fail(const char* ip, uint64_t now) {
    if (!ip || !*ip) return;
    Entry* s = find(ip, now);
    if (!s) {
      const size_t i = freeSlot(now);
      if (i < N) {
        s = &_e[i];
        strncpy(s->ip, ip, LOGIN_RATE_IP_CAP - 1);
      } else {
        s = &overflow(now);
      }
    }
    if (s->fails >= LOGIN_RATE_MAX_FAILS) return;
    s->last = now;
    if (++s->fails >= LOGIN_RATE_MAX_FAILS) s->until = now + LOGIN_RATE_LOCKOUT_MS;
  }

  // Forget ip after a successful login. The overflow entry is shared, so a
  // success never clears it. in: source ip.
  void reset(const char* ip) {
    Entry* s = find(ip, 0);
    if (s) clear(*s);
  }

 private:
  struct Entry {
    char     ip[LOGIN_RATE_IP_CAP];
    uint8_t  fails;
    uint64_t until;  // lockout end while fails is at the limit
    uint64_t last;   // uptime of the latest counted failure
  };
  Entry _e[N] = {};
  Entry _over = {};  // no ip: counts every ip that found no free slot

  // Ages an entry to now: a quiet one is cleared, a passed lockout drops to
  // one below the limit, so the next wrong guess re-locks at once.
  static void age(Entry& c, uint64_t now) {
    if (!c.fails) return;
    if (now >= c.last + LOGIN_RATE_QUIET_MS) {
      clear(c);
    } else if (c.fails >= LOGIN_RATE_MAX_FAILS && now >= c.until) {
      c.fails = LOGIN_RATE_MAX_FAILS - 1;
      c.until = 0;
    }
  }

  // Compares the stored length, so an over-long ip still finds its own slot.
  Entry* find(const char* ip, uint64_t now) {
    if (!ip || !*ip) return nullptr;
    for (size_t i = 0; i < N; i++) {
      if (_e[i].ip[0] && strncmp(_e[i].ip, ip, LOGIN_RATE_IP_CAP - 1) == 0) {
        age(_e[i], now);
        return _e[i].ip[0] ? &_e[i] : nullptr;
      }
    }
    return nullptr;
  }

  // Index of a free or quiet slot, N while every slot holds a live ip. A
  // released lockout is live: it sits one below the limit.
  size_t freeSlot(uint64_t now) {
    for (size_t i = 0; i < N; i++) {
      age(_e[i], now);
      if (!_e[i].ip[0]) return i;
    }
    return N;
  }

  Entry& overflow(uint64_t now) {
    age(_over, now);
    return _over;
  }

  static void clear(Entry& c) { memset(&c, 0, sizeof(c)); }
};
