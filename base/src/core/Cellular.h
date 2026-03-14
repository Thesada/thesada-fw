// thesada-fw — Cellular.h
// SIM7080G Cat-M1/NB-IoT fallback connectivity
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

class Cellular {
public:
  static void begin();
  static void loop();
  static bool connected();
};
