// thesada-fw - boot_wait_policy.h
// Task watchdog budget for blocking waits. A wait feeds once per poll step,
// so no unfed span nears the period, and its total is bounded by clamped
// config. Pure, no Arduino deps, host-unit-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

static const uint32_t TASK_WDT_TIMEOUT_MS = 30000;

// Longest sleep one fed wait step may take.
static const uint32_t WAIT_STEP_MAX_MS = 1000;

static const uint32_t WIFI_POLL_STEP_MS        = 250;
static const int32_t  WIFI_SSID_TIMEOUT_DEF_S  = 10;
static const int32_t  WIFI_SSID_TIMEOUT_MAX_S  = 60;
static const int32_t  WIFI_RETRIES_DEF         = 2;
static const int32_t  WIFI_RETRIES_MAX         = 5;
static const int32_t  WIFI_CANDIDATES_MAX      = 8;

static const uint32_t NTP_POLL_STEP_MS = 200;
static const uint32_t NTP_WAIT_MS      = 15000;

// main.lua / rules.lua: abort after this long, check every N VM instructions.
static const uint32_t LUA_SCRIPT_BUDGET_MS = 10000;
static const int      LUA_HOOK_COUNT       = 1000;

// Telegram and webhook POST after a fed DNS resolve: TCP connect and the write
// (socket timeout each), TLS handshake (core 3 default 120 s), status-line deadline.
static const uint32_t TELEGRAM_IO_MS           = 5000;
static const uint32_t TELEGRAM_HANDSHAKE_S     = 10;
static const uint32_t TELEGRAM_HTTP_TIMEOUT_MS = 5000;

// in: requested sleep. out: the sleep a fed step takes, 1..WAIT_STEP_MAX_MS.
inline uint32_t waitStepMs(uint32_t ms) {
  if (ms < 1) return 1;
  return ms > WAIT_STEP_MAX_MS ? WAIT_STEP_MAX_MS : ms;
}

// in: wifi.timeout_per_ssid_s as configured. out: ms, 1..60 s.
inline uint32_t wifiSsidTimeoutMs(int32_t s) {
  if (s < 1) s = 1;
  if (s > WIFI_SSID_TIMEOUT_MAX_S) s = WIFI_SSID_TIMEOUT_MAX_S;
  return (uint32_t)s * 1000UL;
}

// in: wifi.retries as configured. out: passes over the SSID list, 1..5.
inline uint8_t wifiRetries(int32_t r) {
  if (r < 1) return 1;
  return (uint8_t)(r > WIFI_RETRIES_MAX ? WIFI_RETRIES_MAX : r);
}

// Worst-case connect phase, before NTP. in: raw config and in-range SSIDs.
inline uint32_t wifiConnectBudgetMs(int32_t retries, int32_t ssids, int32_t timeoutS) {
  if (ssids < 0) ssids = 0;
  if (ssids > WIFI_CANDIDATES_MAX) ssids = WIFI_CANDIDATES_MAX;
  return (uint32_t)wifiRetries(retries) * (uint32_t)ssids * wifiSsidTimeoutMs(timeoutS);
}

// Rollover-safe. in: start and now millis, budget. out: true once spent.
inline bool waitBudgetSpent(uint32_t startMs, uint32_t nowMs, uint32_t budgetMs) {
  return (uint32_t)(nowMs - startMs) >= budgetMs;
}
