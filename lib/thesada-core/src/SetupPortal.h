// thesada-fw - SetupPortal.h
// First-boot page on the fallback AP: WiFi, app URL, then the claim link.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>

class SetupPortal {
 public:
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

  // out: claim link, or "" when the apply failed or has not run.
  static const char* claimLink();

  // out: static reason when the apply failed, else "".
  static const char* resultError();
};
