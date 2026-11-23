// thesada-fw - SetupPortal.h
// First-boot page on the fallback AP: WiFi, app URL, then the claim link.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>

class SetupPortal {
 public:
  static constexpr size_t LINK_CAP = 320;

  // Applies a queued form and refreshes whether the page is open.
  // Call from the main loop. in: none. out: none.
  static void loop();

  // out: true when GET/POST /setup should be served.
  static bool open();

  // out: true after submit, until the main loop has applied the form.
  static bool busy();

  // Queue one form. Rejects a bad field, a closed page, or a second submit
  // after a claim link was built. in: ssid, passphrase, app URL. out: queued.
  static bool submit(const char* ssid, const char* pass, const char* url);

  // out: true after a queued form has been applied this boot.
  static bool hasResult();

  // Copy the applied result under its lock, any task. in: link buffer, its cap.
  // out: link ("" when none), *error set to a static reason or "".
  static void result(char* link, size_t cap, const char** error);
};
