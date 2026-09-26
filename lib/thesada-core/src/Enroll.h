// thesada-fw - Enroll.h
// First-boot enrollment over HTTPS: announce, prove the key, poll until the
// owner claims the unit, store the cert, then ack once mTLS is up.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>

class Enroll {
 public:
  // Picks the start step from NVS and registers enroll.status; idle without an
  // identity, enroll.url or claim code. Call after Identity, MQTT and Shell.
  static void begin();

  // Non-blocking tick from the main loop: starts or collects at most one
  // HTTPS request, which runs on its own task.
  static void loop();

  // out: true while enrollment still has work to do this boot.
  static bool active();
};

// Copy the seeded claim code. The caller zeroizes out.
// in: out, cap. out: true when it is 8 digits.
bool enrollLoadClaimCode(char* out, size_t cap);
