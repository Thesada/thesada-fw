// thesada-fw - http_fetch_policy.h
// net.http fetch job: the body preview, the read and wait bounds, who frees
// the job, and one user per modem HTTPS socket. Pure, host-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// First body bytes echoed to the shell. The rest is counted, not kept.
static constexpr size_t HTTP_PREVIEW_CAP = 1024;

// WiFi: body read after the headers, and the shell's wait for the task.
static constexpr uint32_t HTTP_WIFI_READ_MS = 20000;
static constexpr uint32_t HTTP_WIFI_WAIT_MS = 60000;

// Cellular: the read loop stops this long after the connect began, and the
// shell hands the loop back after the wait.
static constexpr uint32_t HTTP_CELL_READ_MS = 25000;
static constexpr uint32_t HTTP_CELL_WAIT_MS = 30000;

// The shell's wait services MQTT this often, so a keepalive ping goes out at
// most this late. The wait outlasts the broker grace without it.
static constexpr uint32_t HTTP_WAIT_TICK_MS = 1000;

// Rollover-safe. in: millis of the last MQTT tick, now. out: tick now.
inline bool httpWaitTickDue(uint32_t lastMs, uint32_t nowMs) {
  return (uint32_t)(nowMs - lastMs) >= HTTP_WAIT_TICK_MS;
}

struct HttpPreview {
  char   data[HTTP_PREVIEW_CAP];
  size_t len;    // bytes kept in data
  size_t total;  // body bytes seen
  bool   cut;    // the read budget ended the GET
};

// Count one body chunk and keep what fits in the preview.
// in: preview, chunk. out: none.
inline void httpPreviewKeep(HttpPreview& p, const uint8_t* buf, size_t n) {
  p.total += n;
  const size_t room = sizeof(p.data) - p.len;
  const size_t copy = n < room ? n : room;
  if (copy > 0) memcpy(p.data + p.len, buf, copy);
  p.len += copy;
}

// Checked by the GET loop each pass, so a stalled body or header stops too.
// in: ms since the connect began, budget ms (0 = none). out: true once spent.
inline bool httpReadBudgetSpent(uint32_t elapsedMs, uint32_t budgetMs) {
  return budgetMs != 0 && elapsedMs >= budgetMs;
}

// Job owner: the shell while it waits, the fetch task once the shell gave up.
enum : uint32_t { HTTP_JOB_RUNNING = 0, HTTP_JOB_DONE = 1, HTTP_JOB_ABANDONED = 2 };

// Task side, its last touch of the job.
// out: true when the shell stopped waiting, so the task frees the job.
inline bool httpJobFinish(std::atomic<uint32_t>& state) {
  return state.exchange(HTTP_JOB_DONE) == HTTP_JOB_ABANDONED;
}

// Shell side, at its wait ceiling.
// out: true when the task finished after all, so the shell keeps the job.
inline bool httpJobAbandon(std::atomic<uint32_t>& state) {
  return state.exchange(HTTP_JOB_ABANDONED) == HTTP_JOB_DONE;
}

// One user of a shared socket at a time, held until it goes out of scope.
class HttpSlotClaim {
 public:
  explicit HttpSlotClaim(std::atomic<uint32_t>& busy)
      : _busy(busy), _held(busy.exchange(1) == 0) {}
  ~HttpSlotClaim() {
    if (_held) _busy.store(0);
  }
  HttpSlotClaim(const HttpSlotClaim&)            = delete;
  HttpSlotClaim& operator=(const HttpSlotClaim&) = delete;
  bool held() const { return _held; }

 private:
  std::atomic<uint32_t>& _busy;
  bool                   _held;
};
